"""Build Unreal material graphs from the source shader descriptors staged by prepare_helgen."""

import math

import unreal


LIB = unreal.MaterialEditingLibrary
PROP = unreal.MaterialProperty
SAMPLE = unreal.MaterialSamplerType


def import_textures(staging, destination, descriptors):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    result = {}
    tasks = []
    for name, info in descriptors.items():
        path = f"{destination}/{name}"
        existing = unreal.EditorAssetLibrary.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
        if existing:
            result[name] = existing
            continue
        task = unreal.AssetImportTask()
        task.filename = str(staging / info["file"])
        task.destination_path = destination
        task.destination_name = name
        task.automated = True
        task.save = False
        tasks.append((name, info, task))
    if tasks:
        tools.import_asset_tasks([task for _, _, task in tasks])
    compressions = unreal.TextureCompressionSettings
    for name, info, task in tasks:
        objects = task.get_objects()
        if len(objects) != 1 or not isinstance(objects[0], unreal.Texture2D):
            raise RuntimeError(f"Could not import source texture {name}")
        texture = objects[0]
        kind = info["kind"]
        texture.set_editor_property("compression_settings", {
            "color": compressions.TC_BC7, "normal": compressions.TC_NORMALMAP,
            "linear": compressions.TC_BC7, "weights": compressions.TC_VECTOR_DISPLACEMENTMAP,
        }[kind])
        texture.set_editor_property("srgb", kind == "color")
        if kind == "weights":
            texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
            texture.set_editor_property("filter", unreal.TextureFilter.TF_BILINEAR)
        unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False)
        result[name] = texture
    return result


class Graph:
    def __init__(self, material, textures, descriptors):
        self.material, self.textures, self.descriptors = material, textures, descriptors
        self.nodes = 0
        self.samples = {}

    def node(self, cls, **props):
        node = LIB.create_material_expression(self.material, cls, -1600 + (self.nodes % 6) * 230,
                                               -380 + (self.nodes // 6) * 190)
        self.nodes += 1
        for key, value in props.items():
            node.set_editor_property(key, value)
        return node

    @staticmethod
    def connect(source, target, pin):
        if not LIB.connect_material_expressions(source[0], source[1], target, pin):
            raise RuntimeError(f"Could not connect {source[1]} to {pin}")

    @staticmethod
    def output(source, prop):
        if not LIB.connect_material_property(source[0], source[1], prop):
            raise RuntimeError(f"Could not connect material output {prop}")

    def scalar(self, value):
        return self.node(unreal.MaterialExpressionConstant, r=float(value)), ""

    def color(self, rgb):
        return self.node(unreal.MaterialExpressionConstant3Vector,
                         constant=unreal.LinearColor(*map(float, rgb), 1.0)), ""

    def sample(self, name, output="RGB", uv=None):
        key = (name, uv[0].get_name() if uv else None)
        if key not in self.samples:
            kind = self.descriptors[name]["kind"]
            sample = self.node(unreal.MaterialExpressionTextureSample, texture=self.textures[name],
                               sampler_type={"color": SAMPLE.SAMPLERTYPE_COLOR, "normal": SAMPLE.SAMPLERTYPE_NORMAL,
                                             "linear": SAMPLE.SAMPLERTYPE_LINEAR_COLOR,
                                             "weights": SAMPLE.SAMPLERTYPE_LINEAR_COLOR}[kind],
                               sampler_source=(unreal.SamplerSourceMode.SSM_CLAMP_WORLD_GROUP_SETTINGS
                                               if kind == "weights" else
                                               unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS))
            if uv:
                self.connect(uv, sample, "")
            self.samples[key] = sample
        return self.samples[key], output

    def multiply(self, a, b):
        expression = self.node(unreal.MaterialExpressionMultiply)
        self.connect(a, expression, "A")
        self.connect(b, expression, "B")
        return expression, ""

    def lerp(self, a, b, weight):
        expression = self.node(unreal.MaterialExpressionLinearInterpolate)
        self.connect(a, expression, "A")
        self.connect(b, expression, "B")
        self.connect(weight, expression, "Alpha")
        return expression, ""

    def custom(self, code, **inputs):
        pins = []
        for name in inputs:
            pin = unreal.CustomInput()
            pin.set_editor_property("input_name", name)
            pins.append(pin)
        expression = self.node(unreal.MaterialExpressionCustom, code=code,
                               output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                               inputs=pins)
        for name, value in inputs.items():
            self.connect(value, expression, name)
        return expression, ""


def terrain_graph(g, entry):
    weights = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=1), ""
    color = normal = roughness = None
    for index, layer in enumerate(entry["terrain"]):
        base = g.sample(layer["base"]) if layer["base"] else g.color((0.27, 0.24, 0.20))
        n = g.sample(layer["normal"]) if layer["normal"] else g.color((0, 0, 1))
        r = g.lerp(g.scalar(0.91), g.scalar(0.58), g.sample(layer["gloss"], "R")) if layer["gloss"] else g.scalar(0.88)
        if index == 0:
            color, normal, roughness = base, n, r
        else:
            offset = index - 1
            alpha = g.sample(entry["weights"][offset // 4], "RGBA"[offset % 4], weights)
            color = g.lerp(color, base, alpha)
            normal = g.lerp(normal, n, alpha)
            roughness = g.lerp(roughness, r, alpha)
    tint = g.node(unreal.MaterialExpressionVertexColor), ""
    g.output(g.multiply(color, tint), PROP.MP_BASE_COLOR)
    normalized = g.node(unreal.MaterialExpressionNormalize)
    g.connect(normal, normalized, "")
    g.output((normalized, ""), PROP.MP_NORMAL)
    g.output(roughness, PROP.MP_ROUGHNESS)
    g.output(g.scalar(0.35), PROP.MP_SPECULAR)


def surface_graph(g, entry, skin_profile):
    material = g.material
    model = entry["model"]
    alpha_test = entry["alphaTest"]
    mask = alpha_test or (entry["alphaBlend"] and model in ("hair", "foliage"))
    translucent = entry["alphaBlend"] and not mask
    material.set_editor_property("two_sided", bool(entry["twoSided"] or model in ("hair", "foliage")))
    if mask:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        material.set_editor_property("opacity_mask_clip_value", min(0.98, max(0.04, entry["cutoff"])))
    elif translucent:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    else:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    if model == "skin" and not translucent:
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_SUBSURFACE_PROFILE)
        material.set_editor_property("subsurface_profile", skin_profile)
    elif model == "foliage":
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)

    uv = None
    if entry["height"]:
        bump = g.node(unreal.MaterialExpressionBumpOffset, height_ratio=0.035)
        g.connect(g.sample(entry["height"], "R"), bump, "Height")
        uv = bump, ""
    base = g.sample(entry["base"], uv=uv) if entry["base"] else g.color((0.4, 0.4, 0.4))
    if entry["tint"]:
        base = g.custom(
            "float3 b = pow(saturate(Base), 1.0 / 2.2); "
            "float3 t = pow(saturate(Tint), 1.0 / 2.2); "
            "return pow(saturate(b * (b + 2.0 * t * (1.0 - b))), 2.2);",
            Base=base, Tint=g.sample(entry["tint"], uv=uv))
    if entry["detail"]:
        detail = g.custom("return clamp(4.0 * pow(saturate(Detail), 1.0 / 2.2), 0.65, 1.5);",
                          Detail=g.sample(entry["detail"], uv=uv))
        base = g.multiply(base, detail)
    if "skinTone" in entry:
        # QNAM is in the same encoded color space as the baked FaceGen tint.
        base = g.custom(
            "float3 b = pow(saturate(Base), 1.0 / 2.2); "
            "float3 c = float3(1.01171875, 0.99609375, 1.01171875) * (b * (b + 2.0 * Tint * (1.0 - b))); "
            "return pow(saturate(c), 2.2);",
            Base=base, Tint=g.color(entry["skinTone"]))
    else:
        color = tuple(max(0.0, min(1.0, x)) for x in entry["color"][:3])
        if color not in ((1, 1, 1), (0, 0, 0)):
            base = g.multiply(base, g.color(color))
    vertex = g.node(unreal.MaterialExpressionVertexColor) if entry["vertexColor"] else None
    if vertex:
        base = g.multiply(base, (vertex, ""))
    g.output(base, PROP.MP_BASE_COLOR)
    if model == "foliage":
        scatter = g.multiply(base, g.color((0.8, 0.94, 0.64)))
        if entry["backlight"]:
            scatter = g.multiply(scatter, g.sample(entry["backlight"], "R", uv=uv))
        g.output(scatter, PROP.MP_SUBSURFACE_COLOR)
    if mask or translucent:
        alpha = g.sample(entry["base"], "A", uv=uv) if entry["base"] else g.scalar(1.0)
        if entry["flags"] & 8 and vertex:
            alpha = g.multiply(alpha, (vertex, "A"))
        if entry["color"][3] < 1:
            alpha = g.multiply(alpha, g.scalar(max(0, entry["color"][3])))
        g.output(alpha, PROP.MP_OPACITY_MASK if mask else PROP.MP_OPACITY)

    if entry["normal"]:
        normal = g.sample(entry["normal"], uv=uv)
        if "basis" in entry:
            material.set_editor_property("tangent_space_normal", False)
            basis = ", ".join("float3({:.8g}, {:.8g}, {:.8g})".format(*axis) for axis in entry["basis"])
            # Skyrim's model-space DDS stores the vertical axis in the green channel.
            normal = g.custom(f"return normalize(mul(Map.xzy * 2.0 - 1.0, float3x3({basis})));", Map=normal)
        g.output(normal, PROP.MP_NORMAL)

    rough = math.pow(2.0 / (max(0, entry["gloss"]) + 2.0), 0.25)
    if model == "eye":
        rough = max(0.23, min(0.32, rough))
    elif model == "hair":
        rough = max(0.40, min(0.54, rough))
        g.output(g.scalar(0.52), PROP.MP_ANISOTROPY)
    elif model == "skin":
        rough = max(0.50, min(0.72, rough))
    else:
        # Keep the source gloss range while accounting for UE's stronger direct specular lobe.
        rough = max(0.42 if entry["environment"] else 0.54, min(0.88, rough))
    roughness = (g.lerp(g.scalar(0.90), g.scalar(rough), g.sample(entry["glossMap"], "R", uv=uv))
                 if entry["glossMap"] else g.scalar(rough))
    g.output(roughness, PROP.MP_ROUGHNESS)
    strength = min(1.0, max(0.0, 0.42 * entry["specStrength"] * max(entry["specular"])))
    specular = g.scalar(strength)
    if entry["spec"]:
        specular = g.multiply(specular, g.sample(entry["spec"], "R", uv=uv))
    if entry["environment"]:
        specular = g.lerp(specular, g.scalar(max(strength, 0.65)), g.sample(entry["environment"], "R", uv=uv))
    g.output(specular, PROP.MP_SPECULAR)
    if entry["glow"] or any(x > 0.0 for x in entry["emission"]):
        emissive = g.color(tuple(max(0, value) for value in entry["emission"]))
        if entry["glow"]:
            multiplier = emissive if any(x > 0.0 for x in entry["emission"]) else g.scalar(1.0)
            emissive = g.multiply(g.sample(entry["glow"], uv=uv), multiplier)
        g.output(emissive, PROP.MP_EMISSIVE_COLOR)


def build_materials(staging, root, manifest, reuse_existing=False):
    descriptors = manifest["textures"]
    folder = f"{root}/Materials"
    textures = import_textures(staging, f"{root}/MaterialTextures", descriptors)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    profile_path = f"{folder}/SP_SkyrimSkin"
    profile_exists = unreal.EditorAssetLibrary.does_asset_exist(profile_path)
    profile = (unreal.EditorAssetLibrary.load_asset(profile_path) if profile_exists else
               tools.create_asset("SP_SkyrimSkin", folder, unreal.SubsurfaceProfile,
                                  unreal.SubsurfaceProfileFactory()))
    if not profile:
        raise RuntimeError("Could not create the skin subsurface profile")
    if not (profile_exists and reuse_existing):
        settings = profile.get_editor_property("settings")
        settings.set_editor_property("enable_burley", True)
        settings.set_editor_property("mean_free_path_distance", 0.35)
        settings.set_editor_property("mean_free_path_color", unreal.LinearColor(1.0, 0.48, 0.36, 1.0))
        settings.set_editor_property("surface_albedo", unreal.LinearColor(0.60, 0.36, 0.28, 1.0))
        settings.set_editor_property("transmission_tint_color", unreal.LinearColor(0.30, 0.24, 0.22, 1.0))
        profile.set_editor_property("settings", settings)
        unreal.EditorAssetLibrary.save_loaded_asset(profile, only_if_is_dirty=False)
    materials = {}
    for name, entry in manifest["materials"].items():
        path = f"{folder}/{name}"
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            material = unreal.EditorAssetLibrary.load_asset(path)
            if reuse_existing and material:
                materials[name] = material
                continue
            LIB.delete_all_material_expressions(material)
        else:
            material = tools.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
        if not material:
            raise RuntimeError(f"Could not create {path}")
        graph = Graph(material, textures, descriptors)
        if "terrain" in entry:
            terrain_graph(graph, entry)
        else:
            surface_graph(graph, entry, profile)
        LIB.layout_material_expressions(material)
        LIB.recompile_material(material)
        unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
        materials[name] = material
    unreal.log(f"HELGEN_MATERIALS {len(materials)} graphs, {len(textures)} textures")
    return materials
