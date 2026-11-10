"""Enable runtime TRI morph rendering on the neutral T5 materials before cooking."""
import unreal
for name in ("M_T5Surface", "M_T5Skin", "M_T5Masked", "M_T5Translucent"):
    material = unreal.load_object(None, f"/T5Data/Runtime/{name}.{name}")
    if not material:
        raise RuntimeError("Missing T5 runtime material " + name)
    material.set_editor_property("used_with_morph_targets", True)
    material.set_editor_property("used_with_skeletal_mesh", True)
    unreal.MaterialEditingLibrary.recompile_material(material)
    if not unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False):
        raise RuntimeError("Cannot save " + name)
    unreal.log("T5_FACIAL_MATERIAL_READY " + name)
