"""Build the exterior WTHR fog material in the Unreal Python commandlet."""

import unreal


ROOT = "/T5Data/Runtime"
NAME = "M_T5WeatherFog"
path = f"{ROOT}/{NAME}"
assets = unreal.EditorAssetLibrary
lib = unreal.MaterialEditingLibrary
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([ROOT])
material = unreal.load_asset(f"{path}.{NAME}")
if material is not None:
    lib.delete_all_material_expressions(material)
else:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        NAME, ROOT, unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
material.set_editor_property("blendable_location", unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)


def node(kind, **properties):
    result = lib.create_material_expression(material, kind)
    for key, value in properties.items():
        result.set_editor_property(key, value)
    return result


scene = node(unreal.MaterialExpressionSceneTexture, scene_texture_id=unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)
depth = node(unreal.MaterialExpressionSceneTexture, scene_texture_id=unreal.SceneTextureId.PPI_SCENE_DEPTH)
world = node(unreal.MaterialExpressionWorldPosition)
camera = node(unreal.MaterialExpressionCameraPositionWS)
params = node(unreal.MaterialExpressionVectorParameter, parameter_name="FogRange", default_value=unreal.LinearColor(0, 100, 1, 0))
color = node(unreal.MaterialExpressionVectorParameter, parameter_name="FogColor", default_value=unreal.LinearColor(0, 0, 0, 1))
far_color = node(unreal.MaterialExpressionVectorParameter, parameter_name="FogFarColor", default_value=unreal.LinearColor(0, 0, 0, 1))
sky = node(unreal.MaterialExpressionVectorParameter, parameter_name="SkyColor", default_value=unreal.LinearColor(0, 0, 0, 1))
opacity = node(unreal.MaterialExpressionScalarParameter, parameter_name="FogOpacity", default_value=0.0)
sources = {"Scene": (scene, "Color"), "Depth": (depth, "Color"), "World": (world, ""), "Camera": (camera, ""),
           "Range": (params, "RGB"), "NearFog": (color, "RGB"), "FarFog": (far_color, "RGB"),
           "Sky": (sky, "RGB"), "Opacity": (opacity, "")}
pins = []
for name in sources:
    pin = unreal.CustomInput()
    pin.set_editor_property("input_name", name)
    pins.append(pin)
fog = node(unreal.MaterialExpressionCustom, inputs=pins, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, code="""
    float3 sceneLinear = pow(saturate(Scene.rgb), 2.2);
    if (Depth.r > 1e7)
        return pow(saturate(lerp(sceneLinear, Sky, saturate(Opacity))), 1.0 / 2.2);
    float x = saturate((distance(World, Camera) - Range.x) / max(Range.y - Range.x, 1.0));
    float alpha = min(saturate(Opacity), pow(x, max(0.01, Range.z)));
    float3 fogLinear = lerp(pow(saturate(NearFog), 2.2), pow(saturate(FarFog), 2.2), alpha);
    return pow(saturate(lerp(sceneLinear, fogLinear, alpha)), 1.0 / 2.2);
""")
for name, (expression, output) in sources.items():
    if not lib.connect_material_expressions(expression, output, fog, name):
        raise RuntimeError(f"Cannot connect fog input {name}")
if not lib.connect_material_property(fog, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
    raise RuntimeError("Cannot connect fog output")
lib.layout_material_expressions(material)
lib.recompile_material(material)
if not assets.save_loaded_asset(material, only_if_is_dirty=False):
    raise RuntimeError(f"Cannot save {path}")
unreal.log("T5_WEATHER_FOG_ASSET_READY")
