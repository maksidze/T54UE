"""Index completed T54Capture pairs; export trajectories in T54UE centimetres. Stdlib only."""
from __future__ import annotations

import argparse
import csv
import html
import json
import math
import re
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[2] / "Saved" / "SkyrimReference"
FRAME_NAME = re.compile(r"frame_[0-9]{8,}\.json\Z")


def to_unreal(position):
    if len(position) != 3 or any(not isinstance(v, (int, float)) or not math.isfinite(v) for v in position):
        raise ValueError("Expected three finite Skyrim coordinates")
    x, y, z = position
    return [y * 100 / 70, x * 100 / 70, z * 100 / 70]


def read_frames(session):
    frames, errors = [], []
    for path in sorted(session.glob("frame_*.json")):
        if not FRAME_NAME.fullmatch(path.name):
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            if data.get("schema") != "t54.skyrim-reference/1":
                raise ValueError("Unsupported frame schema")
            # The basename is derived from the completed JSON; never follow arbitrary image paths.
            expected = path.with_suffix(".png")
            if data.get("image", {}).get("file") != expected.name or not expected.is_file():
                raise ValueError("Missing or mismatched PNG")
            to_unreal(data["player"]["position"])
            frames.append((path, data))
        except (OSError, ValueError, KeyError, TypeError) as exc:
            errors.append(f"{path.name}: {exc}")
    return frames, errors


def csv_safe(value):
    # Prevent spreadsheet formula interpretation of game/mod-provided names.
    if isinstance(value, str) and value.startswith(("=", "+", "-", "@", "\t", "\r", "\n")):
        return "'" + value
    return value


def write_csv(path, fields, rows):
    with path.open("w", newline="", encoding="utf-8-sig") as output:
        writer = csv.writer(output)
        writer.writerow(fields)
        writer.writerows([[csv_safe(value) for value in row] for row in rows])


def make_index(session):
    session = Path(session).resolve()
    frames, errors = read_frames(session)
    if not frames:
        raise ValueError("No completed PNG/JSON pairs in " + str(session))
    rows, ref_rows, cards = [], [], []
    for path, data in frames:
        player = data["player"]
        camera = (data.get("camera") or {}).get("world") or {}
        cell = data.get("cell") or {}
        sky = data.get("sky") or {}
        weather = sky.get("current_weather") or {}
        camera_xyz = to_unreal(camera["position"]) if "position" in camera else [None] * 3
        ue = to_unreal(player["position"])
        rows.append([data["capture_id"], data.get("sample_utc"), data.get("world_generation"), cell.get("form_id"),
                     cell.get("editor_id"), cell.get("name"), *player["position"], *ue, *camera_xyz,
                     (data.get("mq101") or {}).get("stage"), weather.get("form_id"), path.with_suffix(".png").name,
                     data["image"].get("sample_to_present_ms"), data.get("sample_duration_ms")])
        for ref in data.get("references", []):
            base = ref.get("base") or {}
            render = ref.get("render_root") or {}
            render_xyz = to_unreal(render["position"]) if "position" in render else [None] * 3
            ref_rows.append([data["capture_id"], data.get("sample_utc"), data.get("world_generation"),
                             ref.get("form_id"), ref.get("source_plugin"), ref.get("local_form_id"), ref.get("name"),
                             base.get("form_id"), base.get("source_plugin"), base.get("local_form_id"),
                             "actor" in ref, *to_unreal(ref["position"]), *render_xyz])
        title = f'{data["capture_id"]} · {cell.get("editor_id") or cell.get("name") or cell.get("form_id")} · MQ101 {(data.get("mq101") or {}).get("stage", "—")}'
        xyz = ", ".join(f"{v:.1f}" for v in ue)
        cards.append(f'<article><a href="{path.with_suffix(".png").name}"><img loading="lazy" src="{path.with_suffix(".png").name}" alt="{html.escape(title, quote=True)}"></a>'
                     f'<h2>{html.escape(title)}</h2><p>{html.escape(str(data.get("sample_utc", "")))}<br>UE cm: {xyz}</p><a href="{path.name}">JSON</a></article>')
    write_csv(session / "trajectory.csv", ["capture_id", "utc", "world_generation", "cell_form_id", "cell_editor_id", "cell_name",
              "skyrim_x", "skyrim_y", "skyrim_z", "ue_x_cm", "ue_y_cm", "ue_z_cm", "camera_ue_x_cm", "camera_ue_y_cm", "camera_ue_z_cm",
              "mq101_stage", "weather_form_id", "image", "sample_to_present_ms", "sample_duration_ms"], rows)
    write_csv(session / "references.csv", ["capture_id", "utc", "world_generation", "form_id", "plugin", "local_form_id", "name",
              "base_form_id", "base_plugin", "base_local_form_id", "is_actor", "ue_x_cm", "ue_y_cm", "ue_z_cm",
              "render_ue_x_cm", "render_ue_y_cm", "render_ue_z_cm"], ref_rows)
    page = '<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>T54 Skyrim Reference</title>'
    page += '<style>body{background:#171b22;color:#e7edf6;font:16px system-ui;margin:24px}a{color:#9ccaff}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:20px}article{background:#242b36;padding:12px;border-radius:10px}img{width:100%;aspect-ratio:16/9;object-fit:contain;background:black}h2{font-size:16px}p{color:#bdc9da}</style>'
    page += f'<h1>Skyrim → T54UE · {len(frames)} кадров</h1><p>UE cm = [Y, X, Z] × 100/70. <a href="trajectory.csv">Маршрут CSV</a> · <a href="references.csv">Объекты CSV</a></p>'
    if errors:
        page += '<details><summary>Пропущенные кадры</summary><pre>' + html.escape("\n".join(errors)) + '</pre></details>'
    page += '<main>' + "".join(cards) + '</main></html>'
    (session / "index.html").write_text(page, encoding="utf-8")
    return {"frames": len(frames), "reference_rows": len(ref_rows), "skipped": errors, "index": str(session / "index.html")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", nargs="?", type=Path, help="Session directory; latest under --root by default")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args()
    try:
        session = args.session
        if session is None:
            sessions = sorted(p.parent for p in args.root.glob("*/session.json"))
            if not sessions:
                raise ValueError("No recording sessions under " + str(args.root))
            session = sessions[-1]
        print(json.dumps(make_index(session), ensure_ascii=False, indent=2))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, str(exc) + "\n")


if __name__ == "__main__":
    main()
