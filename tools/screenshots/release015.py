"""Capture 0.15's changed UI with the real executable, then reuse the poster template.

python tools/screenshots/release015.py --exe dist/release015/WinShun/WinShun.exe
Use --capture-only to inspect raw renders first, or --export-only to recompose.
The preview modes use temporary profiles; they never stop the running WinShun.
Recording previews briefly show their own demo window and record only its client area.
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

from PIL import Image

import art
import shots


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--capture-only", action="store_true")
    mode.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    work = shots.WORK / "release015"
    work.mkdir(parents=True, exist_ok=True)
    demo = shots.HERE / "scenes/区域销售额.png"
    runs = (
        ("results", "--preview-results", ["--file-metadata", "--gallery-image", str(demo)]),
        ("appearance", "--preview-settings", ["--page", "1"]),
        ("toolbar", "--preview-capture-tools", ["--toolbar-drag"]),
        ("mp4", "--preview-recording", ["--auto"]),
        ("gif", "--preview-recording", ["--auto", "--gif", "--clicks"]),
    )
    if not args.export_only:
        for folder, flag, options in runs:
            print(f"Capturing {folder} with {exe.name}", flush=True)
            subprocess.run([str(exe), flag, str(work / folder), "--scale", "2",
                            "--renderer", "d3d11", *options], check=True)
        (work / "capture.json").write_text(json.dumps({
            "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
            "scale": 2, "renderer": "d3d11", "profiles": "temporary, demo entries only",
        }, indent=2), encoding="utf-8")
    if args.capture_only:
        return

    sources = {
        "panel": ("L-panel", "results/panel-gallery-menu.png"),
        "clipboard-resize": ("L-clipboard-resize", "results/clipboard-gallery.png"),
        "appearance": ("L-appearance", "appearance/p01-t0.png"),
        "recording": ("L-recording", "mp4/ready.png"),
        "recording-gif": ("L-recording-gif", "gif/ready-gif.png"),
        "capture": ("L-capture", "toolbar/toolbar-with-style.png"),
    }
    shots_dir = shots.WORK / "shots"
    masters = work / "poster-masters"
    masters.mkdir(exist_ok=True)
    for shot, raw in sources.values():
        with Image.open(work / raw) as source:
            rgba = source.convert("RGBA")
            if rgba.getchannel("A").getextrema() == (255, 255):
                rgba = art.screen_piece(rgba, radius=16)
            padded = Image.new("RGBA", (rgba.width + 96, rgba.height + 96))
            padded.alpha_composite(rgba, (48, 48))
            padded.save(shots_dir / f"{shot}.png")
    for job in shots.jobs(("readme",), shots_dir, shots.WORK / "out"):
        if job[2].stem not in {*sources, "longshot"}:
            continue
        shots.run(job)
        print("Exported", job[2].name, flush=True)
    # Keep lossless template renders next to the native captures for later reuse.
    for file, (shot, _) in sources.items():
        entry = next(job for job in shots.jobs(("readme",), shots_dir, shots.WORK / "out")
                     if job[2].stem == file)
        image = art.compose(Image.open(shots_dir / f"{shot}.png"), *entry[1][1:])
        image.save(masters / f"{file}.png")
    shutil.copy2(work / "capture.json", masters / "capture.json")


if __name__ == "__main__":
    main()
