"""Setting the stage for the shots: a WinShun profile that indexes only the
demo folder, and Windows' app theme and wallpaper (the windows' Mica takes its
tint from the wallpaper), put back afterwards."""
import ctypes
import json
import os
import pathlib
import shutil
import sys
import tempfile
import time
import winreg

import numpy as np
from PIL import Image

from art import NAMES, background
from capture import Desktop

PROFILE_MARK = "made-for-screenshots.txt"  # a profile folder this may empty again

# The folder the shots show (its name shows in them too, as in the Open
# dialog), a copy of demo/ next to this, and what the demo profile has as
# recently opened, newest first.
DEMO = pathlib.Path("F:/Win顺演示")
RECENT = ("年度报告 定稿 v3.pptx", "财务", "年度报告 数据附录.xlsx", "会议纪要")


def fixed_drives():
    kernel32 = ctypes.WinDLL("kernel32")
    mask = kernel32.GetLogicalDrives()
    roots = [f"{chr(65 + i)}:\\" for i in range(26) if mask >> i & 1]
    return [root for root in roots if kernel32.GetDriveTypeW(root) == 3]  # DRIVE_FIXED


def everything_but(files):
    """Folders to exclude so that only `files` is left of the fixed drives
    (WinShun can't exclude a whole drive): the top folders of every drive,
    and on the way down to `files` the folders beside it."""
    keep = pathlib.Path(files).resolve()
    chain = [keep, *keep.parents]
    excluded = []
    for root in fixed_drives():
        folder = pathlib.Path(root)
        while True:
            inside = next((p for p in chain if p.parent == folder and p != folder), None)  # towards `files`
            try:
                children = [p for p in folder.iterdir() if p.is_dir()]
            except OSError:
                children = []
            excluded += [p for p in children if p != inside]
            if inside is None or inside == keep:
                break
            folder = inside
    return excluded


def ini_list(paths):
    """A QStringList as QSettings writes one to an INI file."""
    def item(text):
        if any(c in text for c in ',";=\\') or text != text.strip():
            return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'
        return text
    return ", ".join(item(p.as_posix()) for p in paths)


def ini_set(text, section, values):
    """The INI text with these keys of `section` set (added where missing)."""
    lines, current, done, out = text.splitlines(), None, set(), []
    def missing():
        return [f"{k}={v}" for k, v in values.items() if k not in done]
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if current == section:
                out += missing()
                done.update(values)
            current = stripped[1:-1]
        elif current == section and "=" in line and line.split("=", 1)[0].strip() in values:
            key = line.split("=", 1)[0].strip()
            out.append(f"{key}={values[key]}")
            done.add(key)
            continue
        out.append(line)
    if current == section:
        out += missing()
    elif not done:
        out += ["", f"[{section}]"] + missing()
    return "\n".join(out) + "\n"


def make_profile(folder, files, recent):
    """A folder for `WinShun.exe --profile <folder>`: this user's settings,
    but nothing indexed outside `files`, no update check, and `recent` (paths,
    newest first) as the recently opened items; no clipboard history yet.
    The demo folder is put in place first if it is not there."""
    folder = pathlib.Path(folder).resolve()
    if pathlib.Path(files) == DEMO and not DEMO.exists():
        shutil.copytree(pathlib.Path(__file__).resolve().parent / "demo", DEMO)
        print(f"demo files copied to {DEMO}")
    if folder.exists() and any(folder.iterdir()):
        if not (folder / PROFILE_MARK).exists():
            sys.exit(f"{folder} is not empty and not a profile made here; not touching it")
        shutil.rmtree(folder)
    (folder / "WinShun").mkdir(parents=True)
    (folder / PROFILE_MARK).write_text("A WinShun profile for screenshots (tools/screenshots).\n")
    real = pathlib.Path(os.environ["APPDATA"]) / "WinShun" / "WinShun.ini"
    text = real.read_text(encoding="utf-8") if real.exists() else ""
    excluded = everything_but(files)
    text = ini_set(text, "Index", {"ExcludedPaths": ini_list(excluded), "IncludeRemovableDrives": "false",
                                   "Folder": ""})
    text = ini_set(text, "Update", {"Automatic": "false"})
    (folder / "WinShun" / "WinShun.ini").write_text(text, encoding="utf-8")
    now = int(time.time() * 1000)
    lines = [f"{pathlib.Path(p)}\t1\t{now - i * 7 * 60_000}" for i, p in enumerate(recent)]
    (folder / "history.txt").write_text("\r\n".join(lines) + ("\r\n" if lines else ""), encoding="utf-8")
    print(f"profile in {folder}: {len(excluded)} folders excluded, {len(recent)} recent items")


SCENE = pathlib.Path(tempfile.gettempdir()) / "winshun-screenshot-scene.json"
PERSONALIZE = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"


def apps_light(value=None):
    """Windows' app theme (1: light), set when a value is given."""
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, PERSONALIZE, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE) as key:
        if value is None:
            return winreg.QueryValueEx(key, "AppsUseLightTheme")[0]
        winreg.SetValueEx(key, "AppsUseLightTheme", 0, winreg.REG_DWORD, value)
    d = Desktop()
    d.user32.SendMessageTimeoutW(0xFFFF, 0x001A, 0, "ImmersiveColorSet", 0x2, 2000, None)  # WM_SETTINGCHANGE
    return value


def wallpaper(path=None):
    """The desktop wallpaper's file, set (for this session only, not saved)
    when a path is given."""
    d = Desktop()
    if path is None:
        name = ctypes.create_unicode_buffer(1024)
        d.user32.SystemParametersInfoW(0x0073, 1024, name, 0)  # SPI_GETDESKWALLPAPER
        return name.value
    d.user32.SystemParametersInfoW(0x0014, 0, ctypes.c_wchar_p(str(path)), 0x2)  # SPI_SETDESKWALLPAPER, send change
    return path


def scene(theme=None, paper=None):
    saved = json.loads(SCENE.read_text(encoding="utf-8")) if SCENE.exists() else {}
    if theme:
        saved.setdefault("apps_light", apps_light())
        apps_light(1 if theme == "light" else 0)
    if paper:
        current = wallpaper()
        if "wallpaper" not in saved and not pathlib.Path(current).is_file():
            sys.exit(f"the wallpaper now is not a picture file ({current!r}); not changing it")
        saved.setdefault("wallpaper", current)
        if paper in NAMES:
            d = Desktop()
            size = (d.user32.GetSystemMetrics(0), d.user32.GetSystemMetrics(1))
            pixels, _ = background(paper, size)
            file = pathlib.Path(tempfile.gettempdir()) / f"winshun-screenshot-{paper}.png"
            Image.fromarray(np.clip(np.round(pixels), 0, 255).astype(np.uint8)).save(file)
            paper = file
        wallpaper(pathlib.Path(paper).resolve())
    SCENE.write_text(json.dumps(saved), encoding="utf-8")
    time.sleep(1.5)  # the wallpaper fades in, and Mica follows
    print(f"scene set; before: {saved}")


def restore():
    if not SCENE.exists():
        print("nothing to restore")
        return
    saved = json.loads(SCENE.read_text(encoding="utf-8"))
    if "apps_light" in saved:
        apps_light(saved["apps_light"])
    if "wallpaper" in saved:
        wallpaper(saved["wallpaper"])
    SCENE.unlink()
    print(f"restored {saved}")
