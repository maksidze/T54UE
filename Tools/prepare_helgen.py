"""Stage full-resolution Skyrim textures and geometry for the Helgen Unreal importer."""

import argparse
from collections import defaultdict
from hashlib import sha256
import json
from pathlib import Path
import re
import struct

import numpy as np
from PIL import Image


VERTEX = np.dtype("<f4")
STRIDE = 48


class Glb:
    def __init__(self, name):
        self.data = bytearray()
        self.doc = {
            "asset": {"version": "2.0", "generator": "T54UE"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}],
            "meshes": [{"name": name, "primitives": []}], "buffers": [{}],
            "bufferViews": [], "accessors": [], "materials": [],
        }

    def accessor(self, values, indices=False, position=False):
        values = np.ascontiguousarray(values, dtype="<u4" if indices else VERTEX)
        self.data.extend(b"\0" * (-len(self.data) % 4))
        self.doc["bufferViews"].append({"buffer": 0, "byteOffset": len(self.data),
                                        "byteLength": values.nbytes, "target": 34963 if indices else 34962})
        self.data.extend(values.tobytes())
        width = values.shape[1] if values.ndim > 1 else 1
        entry = {"bufferView": len(self.doc["bufferViews"]) - 1,
                 "componentType": 5125 if indices else 5126, "count": len(values),
                 "type": {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}[width]}
        if position:
            entry.update(min=values.min(axis=0).tolist(), max=values.max(axis=0).tolist())
        self.doc["accessors"].append(entry)
        return len(self.doc["accessors"]) - 1

    def primitive(self, material, positions, normals, uv, colors, indices, weights_uv=None):
        attributes = {"POSITION": self.accessor(positions, position=True),
                      "NORMAL": self.accessor(normals), "TEXCOORD_0": self.accessor(uv),
                      "COLOR_0": self.accessor(colors)}
        if weights_uv is not None:
            attributes["TEXCOORD_1"] = self.accessor(weights_uv)
        self.doc["materials"].append({"name": material})
        self.doc["meshes"][0]["primitives"].append({
            "attributes": attributes, "indices": self.accessor(indices, indices=True),
            "material": len(self.doc["materials"]) - 1,
        })

    def save(self, path):
        self.data.extend(b"\0" * (-len(self.data) % 4))
        self.doc["buffers"][0]["byteLength"] = len(self.data)
        document = json.dumps(self.doc, separators=(",", ":"), allow_nan=False).encode("utf8")
        document += b" " * (-len(document) % 4)
        with path.open("wb") as target:
            target.write(struct.pack("<4sII", b"glTF", 2, 28 + len(document) + len(self.data)))
            target.write(struct.pack("<I4s", len(document), b"JSON"))
            target.write(document)
            target.write(struct.pack("<I4s", len(self.data), b"BIN\0"))
            target.write(self.data)


def stable_name(prefix, value):
    data = json.dumps(value, separators=(",", ":"), sort_keys=True).encode("utf8")
    return f"{prefix}_{sha256(data).hexdigest()[:14]}"


class TextureStaging:
    def __init__(self, source, output):
        self.source, self.output = source, output
        self.textures, self.cached, self.errors = {}, {}, {}
        (output / "Textures").mkdir(parents=True, exist_ok=True)

    def put(self, name, kind, image):
        filename = f"Textures/{name}.png"
        image.save(self.output / filename, compress_level=3)
        self.textures[name] = {"file": filename, "kind": kind, "size": list(image.size)}
        return name

    def texture(self, path, kind):
        if not path:
            return ""
        if (path, kind) not in self.cached:
            try:
                source = self.source / path
                identity = [path, sha256(source.read_bytes()).hexdigest()]
                with Image.open(source) as image:
                    if kind == "gloss":
                        alpha = image.convert("RGBA").getchannel("A")
                        result = "" if alpha.getextrema() == (255, 255) else self.put(
                            stable_name("T_Gloss", identity), "linear", alpha)
                    else:
                        name = stable_name(f"T_{kind}", identity)
                        result = self.put(name, kind, image.convert("RGBA" if kind == "color" else "RGB"))
            except (OSError, ValueError, NotImplementedError) as error:
                self.errors[path] = str(error)
                result = ""
            self.cached[path, kind] = result
        return self.cached[path, kind]

    def terrain(self, record):
        layers = [record["base"], *[layer["maps"] for layer in record["layers"]]]
        blend = []
        for first in range(0, len(record["layers"]), 4):
            weights = np.zeros((17, 17, 4), dtype=np.uint8)
            for channel, layer in enumerate(record["layers"][first:first + 4]):
                weights[:, :, channel] = np.rint(np.array(layer["weights"]).reshape(17, 17) * 255).astype(np.uint8)
            name = f"T_Weights_{sha256(weights.tobytes()).hexdigest()[:14]}"
            blend.append(self.put(name, "weights", Image.fromarray(weights)))
        return {"terrain": [{"base": self.texture(base, "color"), "normal": self.texture(normal, "normal"),
                             "gloss": self.texture(normal, "gloss")} for base, normal in layers], "weights": blend}

    def material(self, record, group):
        maps = record["maps"]
        shader = record["shader"]
        model_space = "basis" in record
        if shader in (4, 5):
            model = "skin"
        elif shader == 6:
            model = "hair"
        elif shader == 16:
            model = "eye"
        elif shader == 100:
            model = "effect"
        elif group == "Props" and record["twoSided"] and record["alphaTest"]:
            model = "foliage"
        else:
            model = "surface"
        result = dict(record)
        del result["maps"]
        result.update(model=model, base=self.texture(maps[0], "color"),
                      normal=self.texture(maps[1], "linear" if model_space else "normal"),
                      glossMap="" if model_space else self.texture(maps[1], "gloss"),
                      glow=self.texture(maps[2], "color") if shader == 2 else "",
                      height=self.texture(maps[3], "linear") if shader in (3, 7) else "",
                      detail=self.texture(maps[3], "color") if shader == 4 else "",
                      environment=self.texture(maps[5], "linear") if shader in (1, 16) else "",
                      tint=self.texture(maps[6], "color") if shader == 4 else "",
                      backlight=self.texture(maps[7], "linear"),
                      spec=self.texture(maps[2] if model_space and maps[2] else maps[8], "linear"))
        return result


def section_rows(geometry, section):
    return np.frombuffer(geometry, dtype=VERTEX, count=section["vertices"] * 12,
                         offset=section["offset"]).reshape(-1, 12)


def convert(source, output, npc_layout=None):
    manifest = json.loads((source / "meshes.json").read_text(encoding="utf8"))
    if manifest.get("version") != 2:
        raise ValueError("Expected a version 2 Helgen snapshot")
    geometry = (source / "meshes.bin").read_bytes()
    groups = defaultdict(lambda: defaultdict(list))
    materials = {}
    staging = TextureStaging(source, output)
    for section in manifest["sections"]:
        if npc_layout is not None and section["group"] != "NPC":
            continue
        group = section["object"] if section["group"] == "NPC" else section["group"]
        if "terrain" in section:
            material = staging.terrain(section["terrain"])
        else:
            material = staging.material(section["material"], section["group"])
        name = stable_name("SKM", material)
        materials[name] = material
        groups[group][name].append(section)

    assets = []
    for group, by_material in sorted(groups.items()):
        name = re.sub(r"[^a-zA-Z0-9_]", "_", group)
        glb = Glb(name)
        kind = next(iter(by_material.values()))[0]["group"]
        anchor = np.zeros(3, dtype=VERTEX)
        if kind == "NPC":
            if npc_layout is not None:
                anchor = np.array(npc_layout[name], dtype=VERTEX) * np.array([0.7, -0.7, 0.7])
            else:
                points = np.concatenate([section_rows(geometry, s)[:, :3]
                                         for sections in by_material.values() for s in sections])
                lo, hi = points.min(axis=0), points.max(axis=0)
                anchor = (lo + hi) / 2
                anchor[2] = lo[2]
        for material, sections in by_material.items():
            vertices, indices, blend_uv = [], [], []
            base = 0
            for section in sections:
                rows = section_rows(geometry, section)
                vertices.append(rows)
                indices.append(base + np.frombuffer(geometry, dtype="<u4", count=section["indices"],
                                                    offset=section["offset"] + len(rows) * STRIDE))
                if kind == "Terrain":
                    uv = rows[:, :2] - rows[:, :2].min(axis=0)
                    # Texel-centered coordinates for the 17 samples across the 16 terrain quads.
                    blend_uv.append((uv / 128.0 + 0.5) / 17.0)
                base += len(rows)
            rows = np.concatenate(vertices)
            positions = (rows[:, :3] - anchor)[:, [0, 2, 1]] * (1.0 / 70.0)
            positions[:, 2] *= -1
            normals = rows[:, [3, 5, 4]].copy()
            normals[:, 2] *= -1
            glb.primitive(material, positions, normals, rows[:, 6:8], rows[:, 8:12],
                          np.concatenate(indices), np.concatenate(blend_uv) if blend_uv else None)
        glb.save(output / f"{name}.glb")
        objects = {section["object"] for sections in by_material.values() for section in sections}
        assets.append({"name": name, "file": f"{name}.glb", "group": kind, "placements": len(objects),
                       "location": (anchor * np.array([100 / 70, -100 / 70, 100 / 70])).tolist(),
                       "materials": list(by_material)})
    (output / "assets.json").write_text(json.dumps(assets, indent=2), encoding="utf8")
    (output / "materials.json").write_text(json.dumps(
        {"materials": materials, "textures": staging.textures, "texture_errors": staging.errors}, indent=2),
        encoding="utf8")
    print(f"{len(assets)} mesh groups, {len(materials)} materials, {len(staging.textures)} textures")
    for path, error in staging.errors.items():
        print(f"{path}: {error}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--npc-layout", type=Path, help="Existing assets.json for an NPC-only update with fixed pivots")
    args = parser.parse_args()
    layout = None
    if args.npc_layout:
        layout = {entry["name"]: entry["location"] for entry in json.loads(args.npc_layout.read_text(encoding="utf8"))
                  if entry["group"] == "NPC"}
    convert(args.snapshot, args.output, layout)
