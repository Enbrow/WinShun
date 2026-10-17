"""Takes screenshots of WinShun's windows and turns them into images for the
README and for posts: the window on a soft background with a soft shadow under
it, at the size each place wants; and for the first image of a post a cover,
the window turned in perspective under a big title, with a few things by it.
How it all goes together: README.md next to this.

  scene    [--theme light|dark] [--wallpaper NAME|photo.jpg]
           Sets Windows' app theme and the desktop wallpaper for the shots;
           the windows' Mica takes its tint from the wallpaper, so one of the
           backgrounds below makes the window match the image it goes on.
           What was there before is noted; `restore` puts it back.
  profile  [FOLDER] [--files DEMO] [--recent PATH ...] [--number-keys ctrl|alt|off]
           [--number-hints held|always|off]
           A folder (work/profile) for `WinShun.exe --profile FOLDER`: the
           settings of this user, but only DEMO indexed (every other folder
           excluded; F:/Win顺演示 by default, copied from demo/ if missing),
           these recent items and a clipboard history of its own, so no shot
           shows a file of one's own. Quit the running WinShun first.
  shoot    -o window.png [--delay SECONDS] [--winshun]
           The window in front (with WinShun's windows right by it, such as
           the bar under an Open dialog) cut out with its real transparency
           (see capture.py). Run it elevated when WinShun is (the backdrop has
           to go between its window and the rest).
  matte    black.png white.png [black2.png] -o window.png
           The same from shots taken some other way.
  compose  window.png -o out.png [--preset readme|xhs|x|og|square]
           [--bg NAME|photo.jpg] [--title TEXT] [--subtitle TEXT]
           The window as it is: for the README, and for the pages of a post
           after the cover. What is in [brackets] in the title is in colour.
  cover    window.png -o out.png [--preset xhs|x|og|square] [--bg NAME|photo.jpg]
           [--title "LINE\\nLINE"] [--badge "BIG|small" ...] [--keys KEY ...]
           [--tilt YAW PITCH ROLL] [--github NAME]
           The first image of a post: the title, its last line in a gradient
           with a sparkle, badges between laurel branches, GitHub's mark with
           the name on a pill, keys floating by the window, and the window
           turned in 3D, running off the image.
  taskbar  launcher.png screen.png -o out.png [--preset xhs|square] [--title] [--subtitle]
           The post's page on the taskbar box: the launcher over the real
           taskbar from a full-screen shot (where things are on it: TASKBAR
           in art.py).
  sheet    window.png -o sheet.png [--preset ...]
           The window on every background side by side, to choose one.
  render   [readme|pages|square|covers ...] [--shots DIR] [--out DIR]
           Every image again from the shots (work/shots): the README's
           pictures (the square pages, as JPEGs) into docs/images, the
           pages and covers of a post into work/out. All of them when none
           is named.

Usage: python tools/screenshots/shots.py <command> ...   (requires Pillow and numpy)
"""
import argparse
import concurrent.futures
import pathlib
import sys

from PIL import Image

import art
from art import BADGES, NAMES, PRESETS

HERE = pathlib.Path(__file__).resolve().parent
WORK = HERE / "work"

# The README's pictures: the post's square pages (all but the last) and its
# square cover, as JPEGs README_WIDTH wide, named as the pages are without
# their numbers (docs/images/search.jpg ...).
README_WIDTH = 1080

# The pages of a post after the cover: file, shot (several joined with "+":
# short windows one under another), title, subtitle[, background: "silk"
# for a dark one]. With the cover, the taskbar page and the last page,
# 18 images: as many as a Xiaohongshu post takes.
PAGES = (
    ("01-search", "L-search", "打拼音首字母，[秒找文件]", "ndbg → 年度报告，三百多万个文件几毫秒搜完"),
    ("02-numbers", "L-numbers", "按住 Ctrl，[数字直达]", "图标上显示序号，Ctrl+1～9 打开第几行"),
    ("03-newest", "X-newest", "找[最新的那一版]", "Ctrl+S 按修改时间排，最新的或最早的在前"),
    ("04-content", "L-content", "搜[文件里的文字]", "Word、Excel、PPT、PDF、代码，一起搜"),
    ("05-apps", "L-apps", "应用和设置，[拼音直达]", "guanliqi → 任务管理器、设备管理器……"),
    ("06-places", "L-places", "系统设置，[打字就到]", "shipeiqi → 网络连接，一千多个设置页都能搜"),
    ("07-web", "X-web1+X-web3+X-web4", "网站里搜，[一步到位]", "关键词加空格再打字，直接在那个网站上搜"),
    ("08-favorites", "L-fav", "常去的地方，[收藏起来]", "不打字就排在最前；起个关键词，cw 回车就进“财务”"),
    ("09-plugins", "L-plug1+L-plug2+L-plug3", "算、换、查，[打完就有]", "计算、单位换算、颜色……装插件还能更多"),
    ("10-dialog", "L-dialog", "打开、保存时[直接搜]", "对话框下面多一个搜索框，不用一层层点文件夹"),
    ("11-clipboard", "L-clipboard", "剪贴板历史，[随叫随到]", "代替 Win+V · 拼音搜索 · 图片预览"),
    ("12-clipocr", "L-clipocr", "图片里的字，[也能搜]", "复制过的截图，搜“西南”就找到；在本机识别，不联网"),
    ("14-ai", "X-ai2", "让 AI [帮你找文件]", "Claude、Cursor、VS Code 一键连上，只读、按你的权限"),
    ("15-settings", "L-setsearch", "设置[一搜就到]", "打 kjj 就找到“快捷键”，开关直接点"),
    ("16-dark", "D-clipboard", "深色模式，[一样好看]", "跟随系统切换浅色、深色，Win11 云母效果", "silk"),
)
TASKBAR_PAGE = ("13-taskbar", "L-taskbar", "screen-taskbar", "任务栏上的[搜索框]", "点一下就搜，结果从任务栏上方滑出来")
# The README's pictures that the post (18 at most) has no room for: file
# (docs/images/<file>.jpg), shot, title, subtitle.
README_ONLY = (
    ("semantic", "L-semantic", "记不得原词，[按意思找]", "? 去年报销的规定 → 差旅费用管理办法，用你选的模型"),
    ("folder", "L-content-folder", "只搜[一个文件夹]里的字", "第三季度销售额 财务/ → 只在“财务”里找"),
    ("foldermenu", "L-foldermenu", "资源管理器里，[双击空白处]", "收藏的、最近的、开着的文件夹，一下就到"),
    ("appearance", "L-appearance", "外观[随你调]", "主题、透明效果、圆角或直角、界面大小，图标可换单色"),
    ("panel", "L-panel", "结果太多，[展开面板]", "Ctrl+E：左边筛选，右边分列排序"),
    ("preview", "L-preview", "Alt+P，[先看一眼]", "图片、文本、Office、PDF，启动器不关"),
)
ENDING_PAGE = "17-follow"  # who made it, where to get it, follow

# Covers: file, shot, preset, background.
COVERS = (
    ("cover-xhs-light", "L-search", "xhs", "letters"),
    ("cover-xhs-dark", "D-search", "xhs", "silk"),
    ("cover-x", "D-search", "x", "silk"),
    ("og", "L-search", "og", "letters"),
    ("cover-square-light", "L-search", "square", "letters"),
    ("cover-square-dark", "D-search", "square", "silk"),
)
SETS = ("readme", "pages", "square", "covers")


def jobs(sets, shots, out):
    """What `render` makes: (function name, arguments, output) each."""
    shot = lambda name: "+".join(str(shots / f"{part}.png") for part in name.split("+"))
    found = []
    if "readme" in sets:
        images = HERE.parent.parent / "docs/images"
        named = lambda file: images / f"{file.split('-', 1)[1]}.jpg"
        for file, name, title, subtitle, *bg in PAGES:
            found.append(("compose", (shot(name), "square", (bg or ["letters"])[0], title, subtitle), named(file)))
        file, launcher, screen, title, subtitle = TASKBAR_PAGE
        found.append(("taskbar", (shot(launcher), shot(screen), title, subtitle, "square"), named(file)))
        found.append(("cover", (shot("L-search"), "square", "letters"), images / "cover.jpg"))
        for file, name, title, subtitle in README_ONLY:
            found.append(("compose", (shot(name), "square", "letters", title, subtitle), images / f"{file}.jpg"))
    for kind, preset, folder in (("pages", "xhs", "pages"), ("square", "square", "pages-square")):
        if kind in sets:
            for file, name, title, subtitle, *bg in PAGES:
                found.append(("compose", (shot(name), preset, (bg or ["letters"])[0], title, subtitle),
                              out / folder / f"{file}.png"))
            file, launcher, screen, title, subtitle = TASKBAR_PAGE
            found.append(("taskbar", (shot(launcher), shot(screen), title, subtitle, preset), out / folder / f"{file}.png"))
            found.append(("ending", (preset,), out / folder / f"{ENDING_PAGE}.png"))
    if "covers" in sets:
        for file, name, preset, bg in COVERS:
            found.append(("cover", (shot(name), preset, bg), out / "covers" / f"{file}.png"))
    return found


def make(kind, args):
    if kind == "compose":
        window, preset, bg, title, subtitle = args
        parts = window.split("+")
        image = Image.open(window) if len(parts) == 1 else art.stacked([Image.open(p) for p in parts])
        return art.compose(image, preset, bg, title, subtitle)
    if kind == "taskbar":
        launcher, screen, title, subtitle, preset = args
        return art.taskbar_page(Image.open(launcher), Image.open(screen), title, subtitle, preset)
    if kind == "ending":
        return art.ending(*args)
    window, preset, bg = args
    return art.cover(Image.open(window), preset, bg)


def run(job):
    kind, args, out = job
    out.parent.mkdir(parents=True, exist_ok=True)
    image = make(kind, args)
    if out.suffix == ".jpg":  # the README's: smaller
        image = image.convert("RGB").resize((README_WIDTH, round(image.height * README_WIDTH / image.width)), Image.LANCZOS)
        image.save(out, quality=86, optimize=True, progressive=True)
    else:
        image.save(out, optimize=True)
    return out


def render(sets, shots, out):
    todo = jobs(sets or SETS, shots, out)
    missing = sorted({part for _, args, _ in todo for a in args[:2] if isinstance(a, str) and a.endswith(".png")
                      for part in a.split("+") if not pathlib.Path(part).is_file()})
    if missing:
        sys.exit("shots missing (take them first, see README.md):\n  " + "\n  ".join(missing))
    with concurrent.futures.ProcessPoolExecutor() as pool:
        for done in pool.map(run, todo):
            print(f"wrote {done}")


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    c = commands.add_parser("scene")
    c.add_argument("--theme", choices=("light", "dark"))
    c.add_argument("--wallpaper", help=f"{', '.join(NAMES)} or a photo")
    commands.add_parser("restore")
    c = commands.add_parser("profile")
    c.add_argument("folder", nargs="?", default=WORK / "profile")
    c.add_argument("--files", help="the only folder indexed (default: the demo folder)")
    c.add_argument("--recent", nargs="*", help="recently opened, newest first (default: some of the demo folder)")
    c.add_argument("--number-keys", default="ctrl", choices=("ctrl", "alt", "off"),
                   help="the launcher's number keys (default: ctrl, as WinShun's own; off takes the numbers off the icons)")
    c.add_argument("--number-hints", default="held", choices=("held", "always", "off"),
                   help="when the numbers show on the icons (default: held, as WinShun's own: only while Ctrl is held)")
    c = commands.add_parser("shoot")
    c.add_argument("-o", "--out", required=True)
    c.add_argument("--delay", type=float, default=0.0, help="seconds to wait first, to bring the window up")
    c.add_argument("--winshun", action="store_true", help="WinShun's frontmost window, not the one in front")
    c.add_argument("--beside", action="store_true", help="and WinShun's windows against it (the preview)")
    c = commands.add_parser("matte")
    c.add_argument("black")
    c.add_argument("white")
    c.add_argument("check", nargs="?")
    c.add_argument("-o", "--out", required=True)
    for name in ("compose", "sheet"):
        c = commands.add_parser(name)
        c.add_argument("window")
        c.add_argument("-o", "--out", required=True)
        c.add_argument("--preset", choices=PRESETS, default="readme")
        if name == "compose":
            c.add_argument("--bg", default="sky", help=f"{', '.join(NAMES)} or a photo")
            c.add_argument("--title")
            c.add_argument("--subtitle")
    c = commands.add_parser("cover")
    c.add_argument("window")
    c.add_argument("-o", "--out", required=True)
    c.add_argument("--preset", choices=[p for p in PRESETS if PRESETS[p]], default="xhs")
    c.add_argument("--bg", default="silk", help=f"{', '.join(NAMES)} or a photo")
    c.add_argument("--title", default=art.TITLE, help=r"lines split at \n; the last in the gradient")
    c.add_argument("--badge", action="append", help='"BIG|small", between laurels; repeat for more, "" for none')
    c.add_argument("--keys", nargs="*", default=["Ctrl", "Ctrl"], help="keys floating by the window")
    c.add_argument("--tilt", nargs=3, type=float, metavar=("YAW", "PITCH", "ROLL"), help="degrees")
    c.add_argument("--github", default=art.REPOSITORY, help="the name by GitHub's mark, '' for none")
    c = commands.add_parser("taskbar")
    c.add_argument("launcher")
    c.add_argument("screen")
    c.add_argument("-o", "--out", required=True)
    c.add_argument("--preset", choices=[p for p in PRESETS if PRESETS[p]], default="xhs")
    c.add_argument("--bg", default="letters", help=f"{', '.join(NAMES)} or a photo")
    c.add_argument("--title", default=TASKBAR_PAGE[3])
    c.add_argument("--subtitle", default=TASKBAR_PAGE[4])
    c = commands.add_parser("render")
    c.add_argument("sets", nargs="*", metavar="SET", help=f"{', '.join(SETS)}; all when none is named")
    c.add_argument("--shots", type=pathlib.Path, default=WORK / "shots")
    c.add_argument("--out", type=pathlib.Path, default=WORK / "out")
    args = parser.parse_args()

    if args.command in ("scene", "restore", "profile"):
        import stage
        if args.command == "scene":
            return stage.scene(args.theme, args.wallpaper)
        if args.command == "restore":
            return stage.restore()
        files = pathlib.Path(args.files) if args.files else stage.DEMO
        recent = args.recent if args.recent is not None else [stage.DEMO / name for name in stage.RECENT]             if files == stage.DEMO else []
        return stage.make_profile(args.folder, files, recent, args.number_keys, args.number_hints)
    if args.command == "render":
        if unknown := set(args.sets) - set(SETS):
            parser.error(f"no such set: {', '.join(sorted(unknown))} (there are {', '.join(SETS)})")
        return render(args.sets, args.shots, args.out)
    if args.command == "shoot":
        import capture
        image = capture.shoot(args.delay, args.winshun, beside=args.beside)
    elif args.command == "matte":
        import capture
        black, white = Image.open(args.black), Image.open(args.white)
        if args.check and (change := capture.moved(black.convert("RGB"), Image.open(args.check).convert("RGB"))):
            sys.exit(f"the shots differ at {change}: something moved, take them again")
        image = capture.matte(black, white)
    elif args.command == "compose":
        image = art.compose(Image.open(args.window), args.preset, args.bg, args.title, args.subtitle)
    elif args.command == "cover":
        badges = [b for b in args.badge if b] if args.badge is not None else BADGES
        image = art.cover(Image.open(args.window), args.preset, args.bg, args.title, badges, args.keys, args.tilt,
                          github=args.github or None)
    elif args.command == "taskbar":
        image = art.taskbar_page(Image.open(args.launcher), Image.open(args.screen), args.title, args.subtitle,
                                 args.preset, args.bg)
    else:
        image = art.sheet(Image.open(args.window), args.preset)
    image.save(args.out, optimize=True)
    print(f"wrote {args.out} ({image.width}x{image.height})")


if __name__ == "__main__":
    main()
