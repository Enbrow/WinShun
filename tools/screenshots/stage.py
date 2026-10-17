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

# When the demo files were last changed, minutes ago (the dates in the shots
# and the order "newest first" gives); the rest keep theirs.
CHANGED = {
    "年度报告 定稿 v3.pptx": 18, "年度报告 数据附录.xlsx": 75, "年度报告 致辞.docx": 60 * 26,
    "年度报告 提纲.md": 60 * 24 * 3, "年度报告 封面.png": 60 * 24 * 5, "2026 年度报告.docx": 60 * 24 * 9,
    "2025 年度报告.pdf": 60 * 24 * 340, "年度报告素材": 60 * 24 * 2, "财务/年度报表汇总.xlsx": 45,
    "会议纪要/0915 季度复盘.md": 60 * 24 * 26, "董事会汇报 讲稿.txt": 60 * 5,
}

# The demo profile's favorites: kind, path in the demo folder, name, keyword.
FAVORITES = (
    ("folder", "财务", "财务", "cw"),
    ("folder", "会议纪要", "会议纪要", "hy"),
    ("file", "年度报告 定稿 v3.pptx", "年报定稿", "nb"),
    ("folder", "年度报告素材", "年报素材", ""),
)

# The demo clipboard history, newest first: minutes ago and the text; the
# picture (scenes/区域销售额.png, copied in Paint) is the newest.
CLIPS = (
    (3, "第三季度销售额 1260 万，环比增长 9%，华东区首次超过华南"),
    (9, "年度报告 定稿 v3：第 12 页的图表换成柱状图"),
    (24, "下周一 10:00 前提交董事会汇报讲稿"),
    (41, "https://github.com/LingCore/WinShun"),
    (65, "会议改到周五下午 3 点，5 楼大会议室"),
    (130, "SELECT region, SUM(amount) FROM sales GROUP BY region;"),
)

def store_app(package, exe):
    """The program file of a Store app (its folder has the version in it)."""
    import subprocess  # WindowsApps can't be listed, but the package knows where it is
    out = subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-AppxPackage {package}).InstallLocation"],
                         capture_output=True, text=True).stdout.strip().splitlines()
    return str(pathlib.Path(out[0]) / exe) if out else ""


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


def ini_drop(text, *sections):
    """The INI text without these sections."""
    out, skipping = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            skipping = stripped[1:-1] in sections
        if not skipping:
            out.append(line)
    return "\n".join(out) + "\n"


def touch_demo(files):
    """The demo files' dates as CHANGED says, counted back from now."""
    now = time.time()
    for name, minutes in CHANGED.items():
        path = pathlib.Path(files) / name
        if path.exists():
            os.utime(path, (now - minutes * 60, now - minutes * 60))


def write_favorites(folder, files):
    entries = [{"kind": kind, "path": str(pathlib.Path(files) / path), "name": name, **({"keyword": key} if key else {})}
               for kind, path, name, key in FAVORITES]
    (folder / "favorites.json").write_text(json.dumps({"format": "winshun-favorites", "version": 1,
                                                       "favorites": entries}, ensure_ascii=False, indent=2),
                                           encoding="utf-8")


def clip_hash(kind, text):
    """ClipStore's clipHash: SHA-1 of a tag ('f' for files) and the UTF-16."""
    import hashlib
    return hashlib.sha1(("f" if kind == 3 else "t").encode() + text.encode("utf-16-le")).digest()


def seed_clipboard(folder):
    """The demo clipboard history written straight into the profile's
    clipboard.db (as ClipStore keeps it), so nothing goes through the
    system clipboard: Windows' own history, often full, would drop the
    user's oldest entries for it. Run while WinShun is not running."""
    import hashlib
    import sqlite3
    store = folder / "clipboard"
    (store / "images").mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(store / "clipboard.db")
    db.executescript(
        "PRAGMA auto_vacuum=FULL;"
        "CREATE TABLE IF NOT EXISTS clips (id INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER NOT NULL, "
        "text TEXT NOT NULL DEFAULT '', length INTEGER NOT NULL DEFAULT 0, html BLOB, rtf BLOB, "
        "hash BLOB NOT NULL, created INTEGER NOT NULL, used INTEGER NOT NULL, "
        "source TEXT NOT NULL DEFAULT '', sourcePath TEXT NOT NULL DEFAULT '', "
        "groupId INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0, "
        "height INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS groups (id INTEGER PRIMARY KEY, name TEXT NOT NULL, position INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO groups (id, name, position) VALUES (1, '', 0);")
    now = int(time.time() * 1000)
    notepad = store_app("Microsoft.WindowsNotepad", r"Notepad\Notepad.exe")
    rows = []
    for minutes, text in reversed(CLIPS):  # oldest first, as they were copied
        kind = 1 if text.startswith("http") else 0
        at = now - minutes * 60_000
        rows.append((kind, text, len(text.encode("utf-16-le")) // 2, clip_hash(kind, text), at, at, "Notepad", notepad,
                     0, 0, 0))
    picture = pathlib.Path(__file__).resolve().parent / "scenes" / "区域销售额.png"
    png = picture.read_bytes()
    with Image.open(picture) as im:
        size = im.size
    at = now - 60_000
    rows.append((4, "", 0, hashlib.sha1(png).digest(), at, at, "Paint", store_app("Microsoft.Paint", r"PaintApp\mspaint.exe"),
                 size[0], size[1], len(png)))
    for row in rows:
        cur = db.execute("INSERT INTO clips (kind, text, length, hash, created, used, source, sourcePath, width, height, "
                         "bytes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", row)
        if row[0] == 4:
            (store / "images" / f"{cur.lastrowid}.png").write_bytes(png)
    db.commit()
    db.close()
    return len(rows)


def make_profile(folder, files, recent, number_keys="ctrl", number_hints="held"):
    """A folder for `WinShun.exe --profile <folder>`: this user's settings,
    but nothing indexed outside `files`, no update check, the built-in web
    shortcuts only, no shares, `recent` (paths, newest first) as the recently
    opened items, the demo favorites and clipboard history, the launcher's
    number keys `number_keys` (ctrl, alt or off), shown on the icons
    `number_hints` (held, always or off); WinShun's own look whatever this
    user picked (rounded corners, the system's theme, standard size).
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
    text = ini_drop(text, "WebSearch", "Shares")  # one's own sites and shares stay out; the defaults come back
    text = ini_set(text, "Results", {"Sort": "match", "StartList": "recent", "NumberKeys": number_keys,
                                     "NumberHints": number_hints})
    text = ini_set(text, "Appearance", {"Theme": "system", "Transparency": "on", "TaskbarIcon": "color",
                                        "UiSize": "standard", "Language": "system", "Corners": "round"})
    text = ini_set(text, "Launcher", {"DialogAutoJumpAsked": "true"})  # no question under the dialog bar
    # Search by meaning on the demo folder, with this user's model service (and
    # its key, in Windows' credentials); its vectors are the profile's own.
    text = ini_set(text, "Plugin.winshun.semantic", {"Enabled": "true", "Confirmed": "winshun:content, network",
                                                     "Setting.folders": str(pathlib.Path(files)).replace("\\", "\\\\"),
                                                     "Setting.types": "notes"})
    (folder / "WinShun" / "WinShun.ini").write_text(text, encoding="utf-8")
    now = int(time.time() * 1000)
    lines = [f"{pathlib.Path(p)}\t1\t{now - i * 7 * 60_000}" for i, p in enumerate(recent)]
    (folder / "history.txt").write_text("\r\n".join(lines) + ("\r\n" if lines else ""), encoding="utf-8")
    touch_demo(files)
    write_favorites(folder, files)
    clips = seed_clipboard(folder)
    print(f"profile in {folder}: {len(excluded)} folders excluded, {len(recent)} recent items, "
          f"{len(FAVORITES)} favorites, {clips} clips")


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
