"""Run in the Unreal Editor to create the Helgen startup level from staged GLBs."""

import json
import os
import re
import sys
from pathlib import Path

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
from helgen_materials import build_materials


ROOT = "/Game/Helgen"
LEVEL = f"{ROOT}/Helgen"
STAGING = Path(os.environ["T54UE_HELGEN_STAGING"])


def spawn(actor_type, location, rotation, label):
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(actor_type, location, rotation)
    if not actor:
        raise RuntimeError(f"Could not spawn {label}")
    actor.set_actor_label(label)
    return actor


def import_mesh(asset, materials):
    destination = f"{ROOT}/MaterialMeshes/{asset['name']}"
    task = unreal.AssetImportTask()
    task.filename = str(STAGING / asset["file"])
    task.destination_path = destination
    task.automated = True
    task.replace_existing = True
    task.save = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    meshes = [obj for obj in task.get_objects() if isinstance(obj, unreal.StaticMesh)]
    if len(meshes) != 1:
        raise RuntimeError(f"Expected one StaticMesh for {asset['name']}, found {len(meshes)}")
    mesh = meshes[0]
    slots = list(mesh.get_editor_property("static_materials"))
    assigned = set()
    expected = []
    for slot in slots:
        imported = str(slot.get_editor_property("imported_material_slot_name"))
        display = str(slot.get_editor_property("material_slot_name"))
        key = re.search(r"SKM_[0-9a-f]{14}", f"{imported} {display}")
        if not key or key[0] not in asset["materials"]:
            raise RuntimeError(f"Unexpected material slot {imported} / {display} in {asset['name']}")
        slot.set_editor_property("material_interface", materials[key[0]])
        assigned.add(key[0])
        expected.append(materials[key[0]])
    if assigned != set(asset["materials"]):
        raise RuntimeError(f"Missing material slots for {asset['name']}: {set(asset['materials']) - assigned}")
    mesh.set_editor_property("static_materials", slots)

    # Keep translucent effects on the regular static mesh path.
    mesh_editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    nanite = mesh_editor.get_nanite_settings(mesh)
    if nanite.get_editor_property("enabled"):
        nanite.set_editor_property("enabled", False)
        mesh_editor.set_nanite_settings(mesh, nanite, apply_changes=True)
    if asset["group"] != "NPC":
        body = mesh.get_editor_property("body_setup")
        body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    for index, material in enumerate(expected):
        if mesh.get_material(index) != material:
            mesh.set_material(index, material)
        if mesh.get_material(index) != material:
            raise RuntimeError(f"Could not assign {material.get_name()} to {asset['name']} slot {index}")
    if not unreal.EditorAssetLibrary.save_loaded_asset(mesh, only_if_is_dirty=False):
        raise RuntimeError(f"Could not save {asset['name']}")
    return mesh


def main():
    assets = json.loads((STAGING / "assets.json").read_text(encoding="utf8"))
    manifest = json.loads((STAGING / "materials.json").read_text(encoding="utf8"))
    npc_only = os.environ.get("T54UE_HELGEN_UPDATE_NPCS") == "1"
    if npc_only and (not assets or any(asset["group"] != "NPC" for asset in assets)):
        raise RuntimeError("An NPC-only update requires NPC-only staging with the original map pivots")
    materials = build_materials(STAGING, ROOT, manifest, reuse_existing=npc_only)
    meshes = [(asset, import_mesh(asset, materials)) for asset in assets]
    if npc_only:
        unreal.log(f"HELGEN_NPCS_READY {len(meshes)} meshes")
        return
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        if not levels.load_level(LEVEL):
            raise RuntimeError(f"Could not load {LEVEL}")
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        for actor in actors.get_all_level_actors():
            if actor.get_actor_label().startswith("Helgen | "):
                actors.destroy_actor(actor)
    elif not levels.new_level(LEVEL):
        raise RuntimeError(f"Could not create {LEVEL}")

    for asset, mesh in meshes:
        actor = spawn(unreal.StaticMeshActor, unreal.Vector(*asset["location"]), unreal.Rotator(),
                      f"Helgen | {asset['name']}")
        component = actor.static_mesh_component
        component.set_static_mesh(mesh)
        if asset["group"] == "NPC":
            component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        else:
            component.set_collision_profile_name("BlockAll")

    sun = spawn(unreal.DirectionalLight, unreal.Vector(), unreal.Rotator(pitch=-41, yaw=20), "Helgen | Sun")
    sun_component = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_component.set_intensity(52000.0)
    sun_component.set_light_source_angle(1.15)
    sun_component.set_editor_property("atmosphere_sun_light", True)
    fill = spawn(unreal.DirectionalLight, unreal.Vector(), unreal.Rotator(pitch=-26, yaw=-128), "Helgen | Fill")
    fill_component = fill.get_component_by_class(unreal.DirectionalLightComponent)
    fill_component.set_intensity(21000.0)
    fill_component.set_editor_property("cast_shadows", False)
    fill_component.set_editor_property("atmosphere_sun_light", False)
    spawn(unreal.SkyAtmosphere, unreal.Vector(), unreal.Rotator(), "Helgen | Atmosphere")
    sky = spawn(unreal.SkyLight, unreal.Vector(), unreal.Rotator(), "Helgen | Skylight")
    sky_component = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_component.set_editor_property("real_time_capture", True)

    exposure = spawn(unreal.PostProcessVolume, unreal.Vector(), unreal.Rotator(), "Helgen | Exposure")
    exposure.set_editor_property("unbound", True)
    settings = exposure.get_editor_property("settings")
    for limit in ("min", "max"):
        settings.set_editor_property(f"override_auto_exposure_{limit}_brightness", True)
        settings.set_editor_property(f"auto_exposure_{limit}_brightness", 14.0)
    exposure.set_editor_property("settings", settings)

    position = unreal.Vector(-385.0, -195.0, 210.0)
    target = unreal.Vector(96.0, 345.0, 108.0)
    direction = unreal.MathLibrary.find_look_at_rotation(position, target)
    spawn(unreal.PlayerStart, position, direction, "Helgen | Player Start")
    camera = spawn(unreal.CameraActor, position, direction, "Helgen | View")
    camera.set_editor_property("auto_activate_for_player", unreal.AutoReceiveInput.PLAYER0)
    camera.get_component_by_class(unreal.CameraComponent).set_field_of_view(75.0)
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    editor.set_level_viewport_camera_info(position, direction)
    if not levels.save_current_level():
        raise RuntimeError(f"Could not save {LEVEL}")
    for asset, mesh in meshes:
        unreal.log(f"HELGEN_ASSET {asset['name']} {asset['placements']} {mesh.get_path_name()}")
    unreal.log(f"HELGEN_READY {LEVEL}")


main()
