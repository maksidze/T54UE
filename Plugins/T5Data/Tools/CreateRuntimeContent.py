"""Generate the neutral shaders and bootstrap map used by the T5 Data runtime plugin."""

from pathlib import Path
import struct
import tempfile
import zlib

import unreal


ROOT = "/T5Data/Runtime"
MAP = f"{ROOT}/Startup"
LIB = unreal.MaterialEditingLibrary
PROP = unreal.MaterialProperty
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
ASSETS = unreal.EditorAssetLibrary


def neutral_texture(directory, name, srgb):
    path = f"{ROOT}/{name}"
    if ASSETS.does_asset_exist(path):
        return ASSETS.load_asset(path)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    source = Path(directory, f"{name}.png")
    source.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
                       + chunk(b"IDAT", zlib.compress((b"\0" + b"\xff" * 16) * 4)) + chunk(b"IEND", b""))
    task = unreal.AssetImportTask()
    task.filename, task.destination_path, task.automated = str(source), ROOT, True
    TOOLS.import_asset_tasks([task])
    texture = task.get_objects()[0]
    texture.set_editor_property("srgb", srgb)
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
    # The generated PNG is temporary and cannot serve as a reimport source.
    unreal.get_default_object(unreal.SystemLibrary).call_method(
        "SetObjectPropertyByName", args=(texture, "AssetImportData", None)
    )
    ASSETS.save_loaded_asset(texture, only_if_is_dirty=False)
    return texture


class Graph:
    def __init__(self, name, color, linear):
        path = f"{ROOT}/{name}"
        if ASSETS.does_asset_exist(path):
            self.material = ASSETS.load_asset(path)
            LIB.delete_all_material_expressions(self.material)
        else:
            self.material = TOOLS.create_asset(name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
        self.color, self.linear = color, linear

    def node(self, kind, **properties):
        node = LIB.create_material_expression(self.material, kind)
        for key, value in properties.items():
            node.set_editor_property(key, value)
        return node

    def connect(self, source, target, pin):
        if not LIB.connect_material_expressions(source[0], source[1], target, pin):
            raise RuntimeError(f"Could not connect {source[0].get_name()} to {target.get_name()} {pin}")

    def output(self, source, prop):
        if not LIB.connect_material_property(source[0], source[1], prop):
            raise RuntimeError(f"Could not connect {prop}")

    def scalar(self, name, default=0.0):
        return self.node(unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=default), ""

    def vector(self, name, rgb=(1, 1, 1)):
        return self.node(unreal.MaterialExpressionVectorParameter, parameter_name=name,
                         default_value=unreal.LinearColor(*rgb, 1.0)), ""

    def texture(self, name, srgb, uv=None):
        sample = self.node(unreal.MaterialExpressionTextureSampleParameter2D, parameter_name=name,
                           texture=self.color if srgb else self.linear,
                           sampler_type=(unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if srgb else
                                         unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR),
                           sampler_source=(unreal.SamplerSourceMode.SSM_CLAMP_WORLD_GROUP_SETTINGS if uv else
                                           unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS))
        if uv:
            self.connect(uv, sample, "")
        return sample

    def custom(self, code, output_type="FLOAT3", **sources):
        pins = []
        for name in sources:
            pin = unreal.CustomInput()
            pin.set_editor_property("input_name", name)
            pins.append(pin)
        node = self.node(unreal.MaterialExpressionCustom, code=code, inputs=pins,
                         output_type=getattr(unreal.CustomMaterialOutputType, "CMOT_" + output_type))
        for name, source in sources.items():
            self.connect(source, node, name)
        return node, ""

    def finish(self):
        LIB.layout_material_expressions(self.material)
        LIB.recompile_material(self.material)
        if not ASSETS.save_loaded_asset(self.material, only_if_is_dirty=False):
            raise RuntimeError(f"Could not save {self.material.get_name()}")


def surface(name, color, linear, blend, skin=None):
    g = Graph(name, color, linear)
    g.material.set_editor_property("used_with_skeletal_mesh", True)
    g.material.set_editor_property("used_with_morph_targets", True)
    g.material.set_editor_property("blend_mode", blend)
    g.material.set_editor_property("two_sided", blend != unreal.BlendMode.BLEND_OPAQUE)
    g.material.set_editor_property("tangent_space_normal", False)
    if skin:
        g.material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_SUBSURFACE_PROFILE)
        g.material.set_editor_property("subsurface_profile", skin)

    base = g.texture("Base", True)
    vert = g.node(unreal.MaterialExpressionVertexColor)
    color_tint = g.vector("ColorTint")
    g.output(g.custom(
        "float3 base = lerp(float3(.4,.4,.4), Base, HasBase); "
        "float3 b = pow(saturate(base), 1.0/2.2); "
        "float3 t = pow(saturate(Face), 1.0/2.2); "
        "float3 f = pow(saturate(b*(b+2*t*(1-b))), 2.2); "
        "f *= lerp(1.0, clamp(4*pow(saturate(Detail), 1.0/2.2), .65, 1.5), HasDetail); "
        "float3 s = float3(1.01171875,.99609375,1.01171875)*b*(b+2*Skin*(1-b)); "
        "base = lerp(lerp(base * Tint, f, HasFace), pow(saturate(s),2.2), HasSkin); "
        "return base * lerp(1.0, VColor, HasVertex);",
        Base=(base, "RGB"), HasBase=g.scalar("HasBase"), Tint=color_tint,
        Face=(g.texture("FaceTint", True), "RGB"), HasFace=g.scalar("HasFaceTint"),
        Detail=(g.texture("Detail", True), "RGB"), HasDetail=g.scalar("HasDetail"),
        Skin=g.vector("SkinTint"), HasSkin=g.scalar("HasSkinTint"),
        VColor=(vert, ""), HasVertex=g.scalar("HasVertexColor")), PROP.MP_BASE_COLOR)

    normal = g.texture("Normal", False)
    tangent = g.custom("float2 n = (Map.rg * 2-1)*HasNormal; return float3(n, sqrt(saturate(1-dot(n,n))));",
                       Map=(normal, "RGB"), HasNormal=g.scalar("HasNormal"))
    transform = g.node(unreal.MaterialExpressionTransform,
                       transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_TANGENT,
                       transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    g.connect(tangent, transform, "")
    basis = g.scalar("HasBasis")
    g.output(g.custom(
        "float3 m = Map.xzy*2-1; return normalize(lerp(Tangent, m.x*X+m.y*Y+m.z*Z, Basis*HasNormal));",
        Map=(normal, "RGB"), Tangent=(transform, ""), Basis=basis, HasNormal=g.scalar("HasNormal"),
        X=g.vector("Axis0", (1, 0, 0)), Y=g.vector("Axis1", (0, 1, 0)), Z=g.vector("Axis2", (0, 0, 1))), PROP.MP_NORMAL)

    g.output(g.custom("return lerp(lerp(.90, Rough, Gloss), Rough, Basis);", "FLOAT1",
                      Rough=g.scalar("Roughness", .7), Gloss=(normal, "A"), Basis=basis), PROP.MP_ROUGHNESS)
    g.output(g.custom("return lerp(Spec*lerp(1.0, Skin, HasSpec), max(Spec, .65), Env*HasEnv);", "FLOAT1",
                      Spec=g.scalar("Specular", .35), Skin=(g.texture("SkinSpec", False), "R"),
                      HasSpec=g.scalar("HasSkinSpec"), Env=(g.texture("Env", False), "R"),
                      HasEnv=g.scalar("HasEnvironment")), PROP.MP_SPECULAR)
    g.output(g.custom("return lerp(Emission, Glow * (any(Emission>0) ? Emission : 1.0), HasGlow);",
                      Emission=g.vector("Emission", (0, 0, 0)), Glow=(g.texture("Glow", True), "RGB"),
                      HasGlow=g.scalar("HasGlow")), PROP.MP_EMISSIVE_COLOR)

    if blend != unreal.BlendMode.BLEND_OPAQUE:
        masked = blend == unreal.BlendMode.BLEND_MASKED
        code = "float a = Alpha * Tint * lerp(1.0, Vertex, HasVertex); "
        code += "return step(max(Threshold, .002), a);" if masked else "return a;"
        alpha = dict(Alpha=(base, "A"), Tint=(color_tint[0], "A"), Vertex=(vert, "A"),
                     HasVertex=g.scalar("HasVertexAlpha"))
        if masked:
            alpha["Threshold"] = g.scalar("Threshold", .5)
        g.output(g.custom(code, "FLOAT1", **alpha), PROP.MP_OPACITY_MASK if masked else PROP.MP_OPACITY)
    g.finish()


def terrain(color, linear):
    g = Graph("M_T5Terrain", color, linear)
    uv = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=1), ""
    weights = [g.texture(f"Weights{i}", False, uv) for i in range(2)]
    vertex = g.node(unreal.MaterialExpressionVertexColor), ""
    colors, normals = {}, {}
    for i in range(8):
        colors[f"C{i}"] = g.texture(f"Base{i}", True), "RGB"
        n = g.texture(f"Normal{i}", False), "RGB"
        normals[f"N{i}"] = g.custom("float2 n = (Map.rg*2-1)*Enabled; return float3(n,sqrt(saturate(1-dot(n,n))));",
                                    Map=n, Enabled=g.scalar(f"HasNormal{i}"))
    alphas = ["W0.r", "W0.g", "W0.b", "W0.a", "W1.r", "W1.g", "W1.b"]
    weight_inputs = {f"W{i}": g.custom("return float4(RGB, A);", "FLOAT4", RGB=(w, "RGB"), A=(w, "A"))
                     for i, w in enumerate(weights)}
    for prefix, values, output, end in (("C", colors, PROP.MP_BASE_COLOR, "return x * Tint;"),
                                         ("N", normals, PROP.MP_NORMAL, "return normalize(x);")):
        code = f"float3 x = {prefix}0; " + " ".join(
            f"x = lerp(x, {prefix}{i}, {alpha});" for i, alpha in enumerate(alphas, 1)) + " " + end
        if prefix == "C":
            values["Tint"] = vertex
        g.output(g.custom(code, **values, **weight_inputs), output)
    g.output(g.scalar("Roughness", .88), PROP.MP_ROUGHNESS)
    g.finish()


def create_map():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if ASSETS.does_asset_exist(MAP):
        return
    if not levels.new_level(MAP):
        raise RuntimeError(f"Could not create {MAP}")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    def spawn(cls, label, pos=unreal.Vector(), rot=unreal.Rotator()):
        actor = actors.spawn_actor_from_class(cls, pos, rot)
        actor.set_actor_label(label)
        return actor

    spawn(unreal.T5Scene, "T5 Data Loader")
    sun = spawn(unreal.DirectionalLight, "Sun", rot=unreal.Rotator(pitch=-41, yaw=20))
    sun_light = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_light.set_intensity(52000.0)
    sun_light.set_light_source_angle(1.15)
    sun_light.set_editor_property("atmosphere_sun_light", True)
    fill = spawn(unreal.DirectionalLight, "Fill", rot=unreal.Rotator(pitch=-26, yaw=-128))
    fill_light = fill.get_component_by_class(unreal.DirectionalLightComponent)
    fill_light.set_intensity(21000.0)
    fill_light.set_editor_property("cast_shadows", False)
    fill_light.set_editor_property("atmosphere_sun_light", False)
    spawn(unreal.SkyAtmosphere, "Atmosphere")
    sky = spawn(unreal.SkyLight, "Skylight").get_component_by_class(unreal.SkyLightComponent)
    sky.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky.set_editor_property("real_time_capture", True)

    exposure = spawn(unreal.PostProcessVolume, "Exposure")
    exposure.set_editor_property("unbound", True)
    settings = exposure.get_editor_property("settings")
    for limit in ("min", "max"):
        settings.set_editor_property(f"override_auto_exposure_{limit}_brightness", True)
        settings.set_editor_property(f"auto_exposure_{limit}_brightness", 14.0)
    exposure.set_editor_property("settings", settings)
    position = unreal.Vector(-385.0, -195.0, 210.0)
    direction = unreal.MathLibrary.find_look_at_rotation(position, unreal.Vector(96.0, 345.0, 108.0))
    spawn(unreal.PlayerStart, "Player Start", position, direction)
    camera = spawn(unreal.CameraActor, "Helgen View", position, direction)
    camera.set_editor_property("auto_activate_for_player", unreal.AutoReceiveInput.PLAYER0)
    camera.get_component_by_class(unreal.CameraComponent).set_field_of_view(75.0)
    unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(position, direction)
    if not levels.save_current_level():
        raise RuntimeError(f"Could not save {MAP}")


with tempfile.TemporaryDirectory() as directory:
    color = neutral_texture(directory, "T_NeutralColor", True)
    linear = neutral_texture(directory, "T_NeutralLinear", False)
    profile_path = f"{ROOT}/SP_T5Skin"
    profile = ASSETS.load_asset(profile_path) if ASSETS.does_asset_exist(profile_path) else TOOLS.create_asset(
        "SP_T5Skin", ROOT, unreal.SubsurfaceProfile, unreal.SubsurfaceProfileFactory())
    settings = profile.get_editor_property("settings")
    settings.set_editor_property("enable_burley", True)
    settings.set_editor_property("mean_free_path_distance", .35)
    settings.set_editor_property("mean_free_path_color", unreal.LinearColor(1, .48, .36, 1))
    settings.set_editor_property("surface_albedo", unreal.LinearColor(.60, .36, .28, 1))
    settings.set_editor_property("transmission_tint_color", unreal.LinearColor(.30, .24, .22, 1))
    profile.set_editor_property("settings", settings)
    ASSETS.save_loaded_asset(profile, only_if_is_dirty=False)
    modes = unreal.BlendMode
    surface("M_T5Surface", color, linear, modes.BLEND_OPAQUE)
    surface("M_T5Masked", color, linear, modes.BLEND_MASKED)
    surface("M_T5Translucent", color, linear, modes.BLEND_TRANSLUCENT)
    surface("M_T5Skin", color, linear, modes.BLEND_OPAQUE, profile)
    terrain(color, linear)
    create_map()
    unreal.log("T5_RUNTIME_CONTENT_READY")
