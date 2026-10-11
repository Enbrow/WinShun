"""The images: the generated backgrounds, the window lifted off Windows' own
shadow and laid on a background with shadows of its own, the README picture,
the pages of a post (`compose`), its cover (`cover`), the page of the taskbar
box (`taskbar_page`) and a sheet of every background (`sheet`).

Windows' own shadow round a window comes out in the shot too, but cut off
where the shot ends; it is taken off again (`lifted`), and every window, key
and badge gets a shadow made here instead: three layers, each softer and
further down than the one before, tinted with the colour of the ground they
fall on, worked out on a canvas larger than the image, so that none is cut off.

The backgrounds are generated, so they are free to publish: soft blobs of
colour, "silk" (ribbons of light across black) and "letters" (the name in
huge pale letters on peach, in the colours of "dawn"). Posts get a little
grain against banding once a site recompresses them; the README not, as
GitHub shows it as it is and grain would triple the file. A photo of one's
own works too. Text is set in OPPO Sans where installed, else the bundled
Alibaba PuHuiTi.
"""
import math
import os
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
BOLD = ROOT / "resources/fonts/AlibabaPuHuiTi-3-85-Bold.ttf"
REGULAR = ROOT / "resources/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"
# OPPO Sans 4.0 (one variable font, weights 100-700) where it is installed;
# the bundled Alibaba PuHuiTi otherwise.
OPPO = next((f for f in (pathlib.Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/Windows/Fonts",
                         pathlib.Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts")
             for f in [f / "OPPO Sans 4.0.ttf"] if f.is_file()), None)

# Canvas size; None: the window at its own size with a margin round it.
PRESETS = {
    "readme": None,
    "xhs": (1440, 1920),  # Xiaohongshu, 3:4
    "x": (1600, 900),  # X, 16:9
    "og": (1280, 640),  # GitHub's social preview
    "square": (1440, 1440),
}

# Whether it is dark, the base colour, and blobs of colour over it: x and y
# as fractions of the width and height, the radius of the diagonal.
BACKGROUNDS = {
    "dawn": (False, "#F6D2BC", [(0.0, 0.0, 0.50, "#FFB07C"), (1.0, 0.05, 0.45, "#F5A3C7"),
                                (0.05, 1.0, 0.50, "#BBA8F4"), (1.0, 1.0, 0.45, "#FFD08A"),
                                (0.55, 0.45, 0.30, "#FFE8D8")]),
    "sky": (False, "#CFE3FA", [(0.0, 0.0, 0.50, "#8FC2FF"), (1.0, 0.0, 0.45, "#C7D2FF"),
                               (0.0, 1.0, 0.45, "#A8EBE0"), (1.0, 1.0, 0.50, "#B9C8FF"),
                               (0.5, 0.5, 0.30, "#EAF3FF")]),
    "mint": (False, "#DDF2E6", [(0.0, 0.0, 0.50, "#9BE3C0"), (1.0, 0.0, 0.45, "#BFE6F7"),
                                (0.0, 1.0, 0.45, "#E9F5B8"), (1.0, 1.0, 0.50, "#8FD8D0"),
                                (0.5, 0.5, 0.30, "#F2FBF5")]),
    "logo": (False, "#F3F4F8", [(0.0, 0.0, 0.42, "#FFB3AA"), (1.0, 0.0, 0.42, "#9FE3B5"),
                                (0.0, 1.0, 0.42, "#A9CBFF"), (1.0, 1.0, 0.42, "#FFDD94")]),
    "mist": (False, "#ECEFF4", [(0.0, 0.0, 0.50, "#D6E2F0"), (1.0, 0.1, 0.45, "#E6DDF2"),
                                (0.2, 1.0, 0.50, "#DCEDE6"), (1.0, 1.0, 0.40, "#F3E9DD")]),
    "aurora": (True, "#0D1524", [(0.0, 0.0, 0.50, "#1D6A74"), (1.0, 0.0, 0.45, "#3A2A78"),
                                 (0.0, 1.0, 0.45, "#123E6A"), (1.0, 1.0, 0.50, "#1C5A48")]),
    "dusk": (True, "#181329", [(0.0, 0.0, 0.50, "#65305E"), (1.0, 0.1, 0.45, "#2B3C78"),
                               (0.1, 1.0, 0.45, "#9A4A3E"), (1.0, 1.0, 0.50, "#3A2560")]),
    "graphite": (True, "#1A1C20", [(0.0, 0.0, 0.55, "#2E3743"), (1.0, 0.1, 0.45, "#352F40"),
                                   (0.2, 1.0, 0.50, "#24323A"), (1.0, 1.0, 0.40, "#3A3530")]),
}

# The ribbons of "silk": where the middle of each is at the left and at the
# right (fractions of the height), how far it bows down between, how far its
# light reaches (a fraction of the short side), towards which side (-1 up,
# 1 down; the other edge is sharp), and its colours from left to right.
RIBBONS = (
    (0.14, 0.02, -0.06, 0.11, -1, ("#38D3FF", "#3D74FF", "#6A4BFF", "#B57CFF")),
    (0.74, 0.90, 0.08, 0.13, 1, ("#6B3CFF", "#3F5CFF", "#38A6FF", "#A884FF")),
)

# The last line of a cover's title, left to right, on dark and light grounds.
GRADIENT = {True: ("#86DFFF", "#7C8DFF", "#B98BFF"), False: ("#FF7A45", "#F0436F", "#B83DB8")}

# What a cover says unless told otherwise (from the top of the README).
TITLE = "双击 Ctrl\n搜遍整台电脑"
BADGES = ("300 万文件|几毫秒搜完", "免费开源|MIT 许可")
REPOSITORY = "WinShun"  # by GitHub's mark

# Shadows, three layers: how far each is blurred and drops, as fractions of
# how high the thing is lifted off the ground, and how dark it is on a light
# ground and on a dark one.
SHADOW = ((0.12, 0.05, 0.12, 0.28), (0.45, 0.25, 0.10, 0.28), (1.3, 0.9, 0.18, 0.40))


def rgb(colour):
    return np.array(Image.new("RGB", (1, 1), colour).getpixel((0, 0)), np.float32)


def grid(size):
    w, h = size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return x + 0.5, y + 0.5


def ramp(colours, t):
    """The colours spread evenly over t from 0 to 1, blended in between."""
    stops = np.array([rgb(c) for c in colours])
    position = np.clip(t, 0, 1) * (len(colours) - 1)
    i = np.minimum(position.astype(int), len(colours) - 2)
    f = (position - i)[..., None]
    return stops[i] * (1 - f) + stops[i + 1] * f


def mesh(size, base, blobs):
    """The base colour with blobs of colour melting into it and each other."""
    w, h = size
    x, y = grid(size)
    diagonal = (w * w + h * h) ** 0.5
    weight = np.full((h, w), 0.35, np.float32)
    total = weight[..., None] * rgb(base)
    for bx, by, radius, colour in blobs:
        g = np.exp(-((x - bx * w) ** 2 + (y - by * h) ** 2) / (radius * diagonal) ** 2)
        total += g[..., None] * rgb(colour)
        weight += g
    return total / weight[..., None]


def silk(size):
    """Ribbons of light like silk across a ground of near black, one over the
    top and one low down, shading from cyan through blue to violet: each has
    a sharp bright edge, fine threads along it, and fades out on one side."""
    w, h = size
    x, y = grid(size)
    unit = min(w, h)
    u = x / w
    pixels = mesh(size, "#05060D", [(0.0, 0.05, 0.45, "#0A1838"), (1.0, 0.2, 0.40, "#1A1040"),
                                     (0.0, 0.95, 0.45, "#140C34"), (1.0, 1.0, 0.45, "#0C1638")])
    for left, right, bow, reach, side, colours in RIBBONS:
        middle = h * (left + (right - left) * u + bow * np.sin(np.pi * u))
        slope = h * (right - left + bow * np.pi * np.cos(np.pi * u)) / w
        d = side * (y - middle) / np.sqrt(1 + slope ** 2) / unit  # + towards where it fades
        width = reach * (0.8 + 0.3 * np.sin(2.3 * np.pi * u + 7 * left))
        body = np.exp(-np.maximum(d, 0) / width) / (1 + np.exp(np.clip(-d / 0.003, -50, 50)))
        threads = 0.82 + 0.18 * np.sin(d / 0.0045 + 5 * u)
        halo = 0.22 * np.exp(-(d / (0.3 * width)) ** 2)
        edge = 0.55 * np.exp(-(d / 0.002) ** 2)
        light = np.clip((body * threads + halo + edge)[..., None] * ramp(colours, u), 0, 255)
        pixels = 255 - (255 - pixels) * (1 - light / 255)
    return pixels


def letters(size, word="Win顺"):
    """Peach fading into cream downwards, the colours of "dawn" (so that a
    window shot on that matches), the name across it in huge letters a shade
    lighter than the ground, slanting up."""
    w, h = size
    _, y = grid(size)
    pixels = mesh(size, "#FBE2D4", [(0.0, 0.0, 0.50, "#FFC7A6"), (1.0, 0.1, 0.45, "#F9C4DC"),
                                     (0.5, 1.05, 0.55, "#FFF7F1")])
    font = face(BOLD, 0.34 * max(w, h * 0.75))
    pad = Image.new("L", (round(w * 1.7), round(h * 1.7)))
    draw = ImageDraw.Draw(pad)
    step = round(font.size * 0.95)
    for row, top in enumerate(range(-step, pad.height + step, step)):
        draw.text((-(row % 2) * font.size * 1.1, top), "  ".join([word] * 8), font=font, fill=255)
    pad = pad.rotate(10, resample=Image.BICUBIC)
    left, top = (pad.width - w) // 2, (pad.height - h) // 2
    mask = np.asarray(pad.crop((left, top, left + w, top + h)), np.float32) / 255
    mask *= 1 - 0.6 * y / h
    shade = offset(smooth(mask, 0.008 * w), 0.004 * w, 0.007 * w)
    pixels = pixels * (1 - 0.04 * shade[..., None])
    return pixels * (1 - 0.26 * mask[..., None]) + 255 * 0.26 * mask[..., None]


GENERATED = {"silk": (True, silk), "letters": (False, letters)}
NAMES = (*BACKGROUNDS, *GENERATED)


def photo(path, size):
    """The photo cropped to fill the canvas."""
    image = Image.open(path).convert("RGB")
    w, h = size
    scale = max(w / image.width, h / image.height)
    image = image.resize((round(image.width * scale), round(image.height * scale)), Image.LANCZOS)
    left, top = (image.width - w) // 2, (image.height - h) // 2
    return np.asarray(image.crop((left, top, left + w, top + h)), np.float32)


def background(name, size):
    """The canvas as floats, and whether it is dark."""
    if name in GENERATED:
        dark, make = GENERATED[name]
        return make(size), dark
    if name in BACKGROUNDS:
        dark, base, blobs = BACKGROUNDS[name]
        return mesh(size, base, blobs), dark
    pixels = photo(name, size)
    return pixels, float((pixels @ np.array([0.2126, 0.7152, 0.0722], np.float32)).mean()) < 110


def rounded(size, box, radius):
    """How much of each pixel a rounded rectangle (left, top, right, bottom)
    covers, from its distance to the edge: smooth edges, at any fraction of a
    pixel."""
    x, y = grid(size)
    left, top, right, bottom = box
    qx = np.abs(x - (left + right) / 2) - ((right - left) / 2 - radius)
    qy = np.abs(y - (top + bottom) / 2) - ((bottom - top) / 2 - radius)
    distance = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - radius
    return np.clip(0.5 - distance, 0, 1)


def smooth(values, sigma):
    """A Gaussian blur of a float array (three box blurs along each axis),
    what lies beyond its edges taken as 0."""
    if sigma < 0.5:
        return values
    r = max(1, round(((4 * sigma * sigma + 1) ** 0.5 - 1) / 2))
    out = values.astype(np.float64)
    for axis in (0, 1):
        n = out.shape[axis]
        pad = [(0, 0)] * out.ndim
        pad[axis] = (r + 1, r)
        for _ in range(3):
            total = np.cumsum(np.pad(out, pad), axis=axis)
            out = (np.take(total, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
                   - np.take(total, np.arange(n), axis=axis)) / (2 * r + 1)
    return out.astype(np.float32)


def offset(values, dx, dy):
    """The array moved right by dx and down by dy, 0 coming in."""
    dx, dy = round(dx), round(dy)
    h, w = values.shape[:2]
    out = np.zeros_like(values)
    out[max(dy, 0):h + min(dy, 0), max(dx, 0):w + min(dx, 0)] = \
        values[max(-dy, 0):h - max(dy, 0), max(-dx, 0):w - max(dx, 0)]
    return out


def dilated(mask, r):
    """The mask grown by r pixels (a square)."""
    for axis in (0, 1):
        n = mask.shape[axis]
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r, r)
        padded = np.pad(mask, pad)
        grown = np.zeros_like(mask)
        for k in range(2 * r + 1):
            grown |= np.take(padded, np.arange(k, k + n), axis=axis)
        mask = grown
    return mask


def filled(values, known):
    """`values` where they are known, spread in from round about elsewhere:
    blurred known values over the blurred share of known pixels, from near to
    ever further."""
    weight = known.astype(np.float32)
    seen = np.where(known, values, 0).astype(np.float32)
    out, done = seen.copy(), known.copy()
    for sigma in (1.5, 4, 10, 25, 60):
        share = smooth(weight, sigma)
        new = ~done & (share > 0.02)
        out[new] = smooth(seen, sigma)[new] / share[new]
        done |= new
    return out


def window_box(alpha):
    """Where the window itself is: the rows and columns mostly opaque, not its
    shadow nor the logo peeking out from behind it."""
    opaque = alpha >= 0.9
    ys, xs = np.nonzero(opaque.mean(axis=1) > 0.5)[0], np.nonzero(opaque.mean(axis=0) > 0.5)[0]
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def logo_boxes(alpha, box):
    """Where something sticks out well beyond a side of the window (the logo
    peeking out from behind it): from the edge of the image to where the
    window, or the bar beside it, starts."""
    h, w = alpha.shape
    left, top, right, bottom = box
    opaque = alpha >= 0.6
    found = []
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        # The side as lines of pixels going out from the window: depth, along.
        if dy:
            lines = opaque[:top][::-1] if dy < 0 else opaque[bottom:]
        else:
            lines = (opaque[top:bottom, :left][:, ::-1] if dx < 0 else opaque[top:bottom, right:]).T
        far = np.nonzero(lines[4:].any(axis=0))[0]  # not just the edge of a wider window beside
        if far.size < 8:
            continue
        a0, a1 = max(int(far.min()) - 2, 0), min(int(far.max()) + 3, lines.shape[1])

        def reach(i):  # how far the window goes beyond the side, on line i
            if not 0 <= i < lines.shape[1]:
                return 0
            gaps = np.nonzero(~lines[:, i])[0]
            return int(gaps[0]) if gaps.size else lines.shape[0]

        cut = max(reach(a0 - 2), reach(a1 + 1))
        if dy:
            found.append((a0, 0, a1, top - cut) if dy < 0 else (a0, bottom + cut, a1, h))
        else:
            found.append((0, top + a0, left - cut, top + a1) if dx < 0 else (right + cut, top + a0, w, top + a1))
    return found


def lifted(window):
    """The windows without Windows' own shadow round them, cropped, and where
    the window is in that. The shadow is black of some alpha s. A window
    (alpha l, colour L) over it shows as alpha a = s + (1 - s) l and colour
    C = l L / a; the logo peeking out from behind the window is under it
    instead, and shows as the same alpha and C = (1 - s) l L / a. Clear of
    the windows a is s; under their edges, and under the logo (part
    see-through), s is filled in from round about."""
    pixels = np.asarray(window.convert("RGBA"), np.float32)
    a = pixels[..., 3] / 255
    box = window_box(a)
    known = ~dilated(a >= 0.6, 2)  # the shadow is up to 0.5 by a dark window; a gap may be 6 px
    under = np.zeros_like(known)
    for x0, y0, x1, y1 in logo_boxes(a, box):
        known[max(y0 - 3, 0):y1 + 3, max(x0 - 3, 0):x1 + 3] = False
        under[y0:y1, x0:x1] = True
    s = np.minimum(filled(a, known), 0.95)
    l = np.clip((a - s) / (1 - s), 0, 1)
    shown = np.where(under, (1 - s) * l, l)  # how much of L is in C
    colour = np.clip(pixels[..., :3] * (a / np.maximum(shown, 1e-3))[..., None], 0, 255)
    l[l < 0.02] = 0
    ys, xs = np.nonzero(l)
    top, left = ys.min(), xs.min()
    out = np.dstack([colour, l * 255])[top:ys.max() + 1, left:xs.max() + 1]
    image = Image.fromarray(np.round(out).astype(np.uint8), "RGBA")
    return image, (box[0] - left, box[1] - top, box[2] - left, box[3] - top)


# A sprite: premultiplied colour (0-255) and alpha (0-1), as float arrays.

def sprite(image):
    pixels = np.asarray(image.convert("RGBa"), np.float32)
    return pixels[..., :3], pixels[..., 3] / 255


def spots(canvas_shape, piece_shape, x, y):
    """Where a piece with its top left at (x, y) overlaps the canvas: slices
    into the canvas and into the piece, or None."""
    h, w = canvas_shape[:2]
    ph, pw = piece_shape[:2]
    x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + pw, w), min(y + ph, h)
    if x0 >= x1 or y0 >= y1:
        return None
    return (slice(y0, y1), slice(x0, x1)), (slice(y0 - y, y1 - y), slice(x0 - x, x1 - x))


def over(colour, alpha, piece, x, y):
    """Lays a sprite over another (alpha None: an opaque canvas), in place."""
    found = spots(colour.shape, piece[1].shape, round(x), round(y))
    if found:
        to, of = found
        a = piece[1][of]
        colour[to] = colour[to] * (1 - a[..., None]) + piece[0][of]
        if alpha is not None:
            alpha[to] = alpha[to] * (1 - a) + a


def mirrored(piece):
    return piece[0][:, ::-1], piece[1][:, ::-1]


def put(pixels, piece, at, lift=0.0, dark=False, glow=None):
    """Lays a sprite on the canvas with its top left at `at` (anywhere, partly
    off the canvas too) over its shadow: SHADOW's layers for something `lift`
    px above the ground, worked out on a canvas larger by their reach, so
    that whatever runs off the image takes its shadow with it. `glow`: a
    colour it lights the ground with round it. Returns the new canvas."""
    h, w = pixels.shape[:2]
    x, y = round(at[0]), round(at[1])
    if lift > 0:
        m = math.ceil(4.2 * lift) + 2
        cover = np.zeros((h + 2 * m, w + 2 * m), np.float32)
        found = spots(cover.shape, piece[1].shape, x + m, y + m)
        if found:
            cover[found[0]] = piece[1][found[1]]
        if glow is not None:
            light = np.clip(smooth(cover, 1.5 * lift)[m:m + h, m:m + w, None] * rgb(glow) * 0.6, 0, 255)
            pixels = 255 - (255 - pixels) * (1 - light / 255)
        keep = np.ones((h, w), np.float32)
        for spread, drop, light_ground, dark_ground in SHADOW:
            layer = smooth(cover, spread * lift)
            d = round(drop * lift)
            keep *= 1 - (dark_ground if dark else light_ground) * layer[m - d:m - d + h, m:m + w]
        shade = (1 - keep)[..., None]
        pixels = pixels * (1 - shade) + pixels * (pixels / 255) * 0.42 * shade
    over(pixels, None, piece, x, y)
    return pixels


def flat(window, box, width):
    """The window scaled so that its body is `width` px wide, and where the
    body's centre is in it."""
    scale = width / (box[2] - box[0])
    if abs(scale - 1) > 1e-3:
        window = window.convert("RGBa").resize((round(window.width * scale), round(window.height * scale)),
                                               Image.LANCZOS)
    return sprite(window), ((box[0] + box[2]) / 2 * scale, (box[1] + box[3]) / 2 * scale)


def homography(source, target):
    """The coefficients of the perspective map taking the four source points
    to the four target ones, the way PIL takes them."""
    rows, values = [], []
    for (x, y), (u, v) in zip(source, target):
        rows += [[x, y, 1, 0, 0, 0, -u * x, -u * y], [0, 0, 0, x, y, 1, -v * x, -v * y]]
        values += [u, v]
    return tuple(np.linalg.solve(np.array(rows, float), np.array(values, float)))


def tilted(window, box, width, tilt, focal, k=2):
    """The window turned in 3D (degrees about the upright axis, the level one
    and the line of sight) and seen in perspective from `focal` px away, its
    body `width` px wide. Drawn at k times the size and scaled down, so that
    the turned text stays smooth. Returns the sprite, where the body's centre
    is in it, and the body's corners from there."""
    yaw, pitch, roll = np.radians(tilt)
    turn = (np.array([[np.cos(yaw), 0, np.sin(yaw)], [0, 1, 0], [-np.sin(yaw), 0, np.cos(yaw)]])
            @ np.array([[1, 0, 0], [0, np.cos(pitch), -np.sin(pitch)], [0, np.sin(pitch), np.cos(pitch)]])
            @ np.array([[np.cos(roll), -np.sin(roll), 0], [np.sin(roll), np.cos(roll), 0], [0, 0, 1]]))
    scale = width / (box[2] - box[0])
    centre = ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)

    def seen(u, v):
        x, y, z = turn @ np.array([(u - centre[0]) * scale, (v - centre[1]) * scale, 0])
        return focal * x / (focal + z), focal * y / (focal + z)

    corners = [(0, 0), (window.width, 0), (window.width, window.height), (0, window.height)]
    points = [seen(u, v) for u, v in corners]
    left, top = math.floor(min(p[0] for p in points)) - 1, math.floor(min(p[1] for p in points)) - 1
    right, bottom = math.ceil(max(p[0] for p in points)) + 1, math.ceil(max(p[1] for p in points)) + 1
    target = [((x - left) * k, (y - top) * k) for x, y in points]
    image = window.convert("RGBa").transform(((right - left) * k, (bottom - top) * k), Image.PERSPECTIVE,
                                             homography(target, corners), Image.BICUBIC).reduce(k)
    body = [seen(u, v) for u, v in ((box[0], box[1]), (box[2], box[1]), (box[2], box[3]), (box[0], box[3]))]
    return sprite(image), (-left, -top), body


def face(path, size):
    """A font of the given size: BOLD or REGULAR, in OPPO Sans if there."""
    if OPPO is None:
        return ImageFont.truetype(str(path), max(1, round(size)))
    font = ImageFont.truetype(str(OPPO), max(1, round(size)))
    font.set_variation_by_axes([700 if path == BOLD else 400])
    return font


def text_piece(text, font, fill):
    """Text as a sprite, its ink from (2, 2), filled with a colour or with a
    gradient (a tuple of colours, left to right)."""
    left, top, right, bottom = font.getbbox(text)
    mask = Image.new("L", (right - left + 4, bottom - top + 4))
    ImageDraw.Draw(mask).text((2 - left, 2 - top), text, font=font, fill=255)
    alpha = np.asarray(mask, np.float32) / 255
    if isinstance(fill, tuple):
        x, _ = grid(mask.size)
        colour = ramp(fill, (x - 2) / max(right - left, 1))
    else:
        colour = np.broadcast_to(rgb(fill), alpha.shape + (3,))
    return colour * alpha[..., None], alpha


def sparkle(radius, colour="#FFFFFF"):
    """A four-pointed star with curved sides, a soft glow round it; its centre
    in the middle of the sprite."""
    size, k = math.ceil(radius * 3.2), 4
    t = np.linspace(0, 2 * np.pi, 720, endpoint=False)
    c, s = np.cos(t), np.sin(t)
    xs = size * k / 2 + radius * k * 0.9 * np.sign(c) * np.abs(c) ** 2.6
    ys = size * k / 2 + radius * k * 1.1 * np.sign(s) * np.abs(s) ** 2.6
    mask = Image.new("L", (size * k, size * k))
    ImageDraw.Draw(mask).polygon(list(zip(xs, ys)), fill=255)
    star = np.asarray(mask.reduce(k), np.float32) / 255
    alpha = 1 - (1 - star) * (1 - 0.55 * smooth(star, 0.3 * radius))
    return alpha[..., None] * rgb(colour), alpha


def keycap(text, height, dark):
    """A key of a keyboard seen from the front and a little above: its top
    face rising from the sides, lit from above, the name on it."""
    font = face(BOLD, height * 0.3)
    width = max(round(height * 1.3), round(font.getlength(text) + height * 0.75))
    size = (width, height)
    _, y = grid(size)
    radius = height * 0.22
    body = rounded(size, (0, 0, width, height), radius)
    inset = height * 0.075
    top_box = (inset, inset * 0.55, width - inset, height * 0.85)
    top = rounded(size, top_box, radius * 0.8)[..., None]
    across = (y - top_box[1]) / (top_box[3] - top_box[1])
    if dark:
        side, faces, ink, gleam = ramp(("#2B303D", "#111218"), y / height), ramp(("#454C60", "#2D3240"), across), \
            "#F2F4FA", 0.25
    else:
        side, faces, ink, gleam = ramp(("#DDE1E9", "#A8AFBD"), y / height), ramp(("#FFFFFF", "#ECEFF4"), across), \
            "#3C4252", 0.9
    colour = side * (1 - top) + faces * top
    edge = np.clip(top[..., 0] - rounded(size, (top_box[0], top_box[1] + max(1.5, height * 0.014), top_box[2],
                                                top_box[3]), radius * 0.8), 0, 1)[..., None]
    colour = colour * (1 - gleam * edge) + 255 * gleam * edge
    left, t, right, b = font.getbbox(text)
    mask = Image.new("L", size)
    middle = ((top_box[0] + top_box[2]) / 2, (top_box[1] + top_box[3]) / 2)
    ImageDraw.Draw(mask).text((middle[0] - (left + right) / 2, middle[1] - (t + b) / 2), text, font=font, fill=255)
    ink_cover = np.asarray(mask, np.float32)[..., None] / 255
    colour = colour * (1 - ink_cover) + rgb(ink) * ink_cover
    return Image.fromarray(np.round(np.dstack([np.clip(colour, 0, 255), body * 255])).astype(np.uint8), "RGBA")


def turned(image, degrees):
    """An image turned about its centre (counterclockwise), as a sprite."""
    return sprite(image.convert("RGBa").rotate(degrees, resample=Image.BICUBIC, expand=True))


def laurel(height, colour):
    """A laurel branch curving up the left of a badge `height` px high, as a
    sprite."""
    k = 4
    big = height * k
    cx, cy, radius = 0.62 * big, 0.5 * big, 0.42 * big
    shapes = []

    def leaf(base, direction, length, width):
        d = direction / np.linalg.norm(direction)
        across = np.array([-d[1], d[0]])
        s = np.linspace(0, 1, 32)
        half = width / 2 * np.sin(np.pi * s) ** 0.8 * (1 - 0.25 * s)
        spine = base + np.outer(s * length, d)
        return np.vstack([spine + np.outer(half, across), (spine - np.outer(half, across))[::-1]])

    def at(angle):  # a point on the arc, and the way up along it and out from it
        return (np.array([cx + radius * np.cos(angle), cy - radius * np.sin(angle)]),
                np.array([np.sin(angle), np.cos(angle)]), np.array([np.cos(angle), -np.sin(angle)]))

    angles = np.radians(np.linspace(246, 128, 7))
    for i, angle in enumerate(angles):
        point, up, out = at(angle)
        grow = 1 - 0.2 * i / (len(angles) - 1)
        shapes.append(leaf(point, up * np.cos(0.72) + out * np.sin(0.72), 0.23 * big * grow, 0.1 * big * grow))
        if 0 < i:
            shapes.append(leaf(point, up * np.cos(0.6) - out * np.sin(0.6), 0.15 * big * grow, 0.075 * big * grow))
    point, up, _ = at(np.radians(122))
    shapes.append(leaf(point, up, 0.17 * big, 0.08 * big))
    stem = [at(a)[0] for a in np.radians(np.linspace(252, 122, 40))]
    points = np.vstack(shapes + [np.array(stem)])
    shift = -points.min(axis=0) + 2 * k
    w = math.ceil((points[:, 0].max() + shift[0] + 2 * k) / k) * k
    mask = Image.new("L", (w, math.ceil(big / k) * k))
    draw = ImageDraw.Draw(mask)
    draw.line([tuple(p + shift) for p in stem], fill=255, width=max(1, round(0.024 * big)), joint="curve")
    for shape in shapes:
        draw.polygon([tuple(p + shift) for p in shape], fill=255)
    alpha = np.asarray(mask.reduce(k), np.float32) / 255
    if isinstance(colour, tuple):  # top to bottom
        return alpha[..., None] * ramp(colour, grid(alpha.shape[::-1])[1] / alpha.shape[0]), alpha
    return alpha[..., None] * rgb(colour), alpha


def badge(big, small, height, colour, leaves):
    """Two lines of text between laurel branches (`leaves`: their colours,
    top to bottom), as a sprite."""
    branch = laurel(height, leaves)
    texts = [text_piece(big, face(BOLD, height * 0.34), colour)]
    if small:
        texts.append(text_piece(small, face(BOLD, height * 0.2), colour))
    gap = round(height * 0.04)
    inner = max(t[1].shape[1] for t in texts)
    bw = branch[1].shape[1]
    w = 2 * bw + 2 * gap + inner
    colours, alpha = np.zeros((height, w, 3), np.float32), np.zeros((height, w), np.float32)
    over(colours, alpha, branch, 0, 0)
    over(colours, alpha, mirrored(branch), w - bw, 0)
    spacing = round(height * 0.05)
    block = sum(t[1].shape[0] for t in texts) + spacing * (len(texts) - 1)
    y = (height - block) / 2 - height * 0.02
    for t in texts:
        over(colours, alpha, t, (w - t[1].shape[1]) / 2, y)
        y += t[1].shape[0] + spacing
    return colours, alpha


# GitHub's mark (Octicons' mark-github, 16x16).
GITHUB = ("M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49"
          "-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52"
          ".28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18"
          " 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07"
          "-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42"
          "-3.58-8-8-8z")


def outline(d, steps=16):
    """The subpaths of an SVG path (lines, cubic Béziers, arcs) as polygons."""
    import re
    number = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
    i = 0

    def skip():
        nonlocal i
        while i < len(d) and d[i] in " ,\t\r\n":
            i += 1

    def num():
        nonlocal i
        skip()
        m = number.match(d, i)
        i = m.end()
        return float(m.group())

    def flag():  # one character, as in "0016" (large 0, sweep 0, x 16)
        nonlocal i
        skip()
        i += 1
        return d[i - 1] == "1"

    def arc(x1, y1, rx, ry, phi, large, sweep, x2, y2):  # SVG 1.1, F.6.5
        c, s = math.cos(phi), math.sin(phi)
        dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
        x1p, y1p = c * dx + s * dy, -s * dx + c * dy
        scale = max(1.0, math.sqrt(x1p ** 2 / rx ** 2 + y1p ** 2 / ry ** 2))
        rx, ry = rx * scale, ry * scale
        k = math.sqrt(max(0.0, (rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p)
                          / (rx * rx * y1p * y1p + ry * ry * x1p * x1p)))
        k = -k if large == sweep else k
        cxp, cyp = k * rx * y1p / ry, -k * ry * x1p / rx
        cx, cy = c * cxp - s * cyp + (x1 + x2) / 2, s * cxp + c * cyp + (y1 + y2) / 2
        start = math.atan2((y1p - cyp) / ry, (x1p - cxp) / rx)
        turn = math.atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx) - start
        if sweep and turn < 0:
            turn += 2 * math.pi
        elif not sweep and turn > 0:
            turn -= 2 * math.pi
        return [(cx + rx * c * math.cos(t) - ry * s * math.sin(t), cy + rx * s * math.cos(t) + ry * c * math.sin(t))
                for t in start + turn * np.linspace(0, 1, steps + 1)[1:]]

    shapes, points, x, y, start, command, control = [], [], 0.0, 0.0, (0.0, 0.0), None, None
    while True:
        skip()
        if i >= len(d):
            break
        if d[i].isalpha():
            command = d[i]
            i += 1
        ox, oy = (x, y) if command.islower() else (0.0, 0.0)
        kind, previous, control = command.upper(), control, None
        if kind == "M":
            if len(points) > 2:
                shapes.append(points)
            x, y = ox + num(), oy + num()
            points, start = [(x, y)], (x, y)
            command = "l" if command == "m" else "L"  # pairs after it are lines
        elif kind == "Z":
            x, y = start
        elif kind in "LHV":
            x = ox + num() if kind != "V" else x
            y = oy + num() if kind != "H" else y
            points.append((x, y))
        elif kind in "CS":
            if kind == "C":
                c1 = (ox + num(), oy + num())
            else:  # the last control point mirrored
                c1 = (2 * x - previous[0], 2 * y - previous[1]) if previous else (x, y)
            c2 = (ox + num(), oy + num())
            end = (ox + num(), oy + num())
            t = np.linspace(0, 1, steps + 1)[1:, None]
            curve = ((1 - t) ** 3 * np.array((x, y)) + 3 * (1 - t) ** 2 * t * np.array(c1)
                     + 3 * (1 - t) * t * t * np.array(c2) + t ** 3 * np.array(end))
            points += [tuple(p) for p in curve]
            (x, y), control = end, c2
        elif kind == "A":
            rx, ry, phi = num(), num(), math.radians(num())
            large, sweep = flag(), flag()
            end = (ox + num(), oy + num())
            points += arc(x, y, abs(rx), abs(ry), phi, large, sweep, *end)
            x, y = end
        else:
            raise ValueError(f"path command {command!r} not handled")
    if len(points) > 2:
        shapes.append(points)
    return [np.array(p) for p in shapes]


def github_mark(height, colour, k=4):
    """GitHub's mark `height` px high, as a sprite (even-odd fill)."""
    size = math.ceil(height)
    hits = np.zeros((size * k, size * k), bool)
    for shape in outline(GITHUB):
        mask = Image.new("L", (size * k, size * k))
        ImageDraw.Draw(mask).polygon([tuple(p) for p in shape * height * k / 16], fill=255)
        hits ^= np.asarray(mask) > 127
    alpha = np.asarray(Image.fromarray(hits.astype(np.uint8) * 255).reduce(k), np.float32) / 255
    return alpha[..., None] * rgb(colour), alpha


def github_chip(name, height, dark):
    """GitHub's mark and the repository's name on a pill, as a sprite: dark on
    a light ground, light on a dark one."""
    fill, ink = ("#F6F8FA", "#1F2328") if dark else ("#1F2328", "#FFFFFF")
    logo = github_mark(height * 0.56, ink)
    text = text_piece(name, face(BOLD, height * 0.44), ink)
    pad, gap = height * 0.34, height * 0.2
    w = round(2 * pad + logo[1].shape[1] + gap + text[1].shape[1] - 4)
    alpha = rounded((w, height), (0, 0, w, height), height / 2)
    colours = alpha[..., None] * rgb(fill)
    over(colours, alpha, logo, pad, (height - logo[1].shape[0]) / 2)
    over(colours, alpha, text, pad + logo[1].shape[1] + gap - 2, (height - text[1].shape[0]) / 2)
    return colours, alpha


def maker(height, dark):
    """LingCore's logo with its name beside it, as a sprite."""
    logo = sprite(Image.open(ROOT / "resources/lingcore.png").convert("RGBA").resize(
        (round(height), round(height)), Image.LANCZOS))
    name = text_piece("LingCore", face(BOLD, height * 0.5), "#F4F5F8" if dark else "#15171D")
    gap = round(height * 0.22)
    w = round(height) + gap + name[1].shape[1]
    colours, alpha = np.zeros((round(height), w, 3), np.float32), np.zeros((round(height), w), np.float32)
    over(colours, alpha, logo, 0, 0)
    over(colours, alpha, name, round(height) + gap, (height - name[1].shape[0]) / 2)
    return colours, alpha


def mark(height, dark):
    """The logo with the name beside it, as a sprite."""
    sys.path.insert(0, str(ROOT / "tools"))
    import make_icon
    logo = sprite(make_icon.render(round(height)))
    name = text_piece("Win顺", face(BOLD, height * 0.6), "#F4F5F8" if dark else "#15171D")
    gap = round(height * 0.22)
    w = round(height) + gap + name[1].shape[1]
    colours, alpha = np.zeros((round(height), w, 3), np.float32), np.zeros((round(height), w), np.float32)
    over(colours, alpha, logo, 0, 0)
    over(colours, alpha, name, round(height) + gap, (height - name[1].shape[0]) / 2)
    return colours, alpha


def title_block(canvas, top, title, subtitle, ink, soft, em, accent=None):
    """Draws the title and subtitle centred from `top` down, the title `em`
    px high, what is in [brackets] in it in the `accent` gradient; returns
    the y under them."""
    draw = ImageDraw.Draw(canvas)
    y = top
    for text, path, size, colour, gap in ((title, BOLD, 1.0, ink, 0.32), (subtitle, REGULAR, 0.48, soft, 0.0)):
        if not text:
            continue
        font = face(path, em * size)
        for line in text.replace("\\n", "\n").split("\n"):
            runs = [(run, i % 2 == 1) for i, run in enumerate(line.replace("]", "[").split("["))]
            left, line_top, right, _ = draw.textbbox((0, 0), "".join(run for run, _ in runs), font=font)
            x, base = (canvas.width - (right - left)) / 2 - left, y - line_top
            for run, accented in runs:
                if accented and accent:
                    mask = Image.new("L", canvas.size)
                    ImageDraw.Draw(mask).text((x, base), run, font=font, fill=255)
                    across, _ = grid(canvas.size)
                    paint = ramp(accent, (across - x) / max(font.getlength(run), 1))
                    canvas.paste(Image.fromarray(np.round(paint).astype(np.uint8)), mask=mask)
                elif run:
                    draw.text((x, base), run, font=font, fill=colour)
                x += font.getlength(run)
            y += round(font.size * 1.25)
        y += round(font.size * gap)
    return y


def grain(pixels, seed):
    return pixels + np.random.default_rng(seed).normal(0, 1.4, pixels.shape[:2] + (1,)).astype(np.float32)


def compose(window, preset="readme", bg="sky", title=None, subtitle=None, seed=1):
    window, box = lifted(window)
    body_w, body_h = box[2] - box[0], box[3] - box[1]
    size = PRESETS[preset]
    titled = bool(title or subtitle) and size is not None
    if size is None:
        margin = round(0.09 * body_w)
        size = (body_w + 2 * margin, body_h + 2 * margin)
        width, middle, title_top = body_w, (size[0] / 2, size[1] / 2), 0
    else:
        # The title near the top, under the logo and name, and the window as
        # large as fits under it (a little larger than shot at most), in the
        # middle of the room left.
        w, h = size
        em = 0.075 * min(w, 1.1 * h)
        title_top = round(0.145 * h)
        title_h = title_block(Image.new("RGB", size), 0, title, subtitle, 0, 0, em) \
            if titled else 0
        gap = 0.045 * h if titled else 0
        room = (w - 0.12 * w, h - title_top - title_h - gap - 0.06 * h)
        width = min(room[0], room[1] * body_w / body_h, 1.2 * body_w)
        spare = room[1] - width * body_h / body_w
        middle = (w / 2, title_top + title_h + gap + 0.5 * spare + width * body_h / body_w / 2)

    pixels, dark = background(bg, size)
    piece, centre = flat(window, box, width)
    pixels = put(pixels, piece, (middle[0] - centre[0], middle[1] - centre[1]))  # no shadow under the program
    if preset != "readme":
        pixels = put(pixels, mark(0.06 * min(size), dark), (0.06 * min(size), 0.055 * min(size)))
        made = maker(0.06 * min(size), dark)
        pixels = put(pixels, made, (size[0] - 0.06 * min(size) - made[1].shape[1], 0.055 * min(size)))
        pixels = grain(pixels, seed)
    canvas = Image.fromarray(np.clip(np.round(pixels), 0, 255).astype(np.uint8))
    if titled:
        ink, soft = ("#F5F5F7", "#C9CBD3") if dark else ("#1B1B1F", "#4A4D57")
        title_block(canvas, title_top, title, subtitle, ink, soft, em, GRADIENT[dark])
    if preset == "readme":
        corners = rounded(size, (0, 0, size[0], size[1]), 0.022 * size[0])
        canvas.putalpha(Image.fromarray(np.round(corners * 255).astype(np.uint8)))
    return canvas


def cover(window, preset="xhs", bg="silk", title=TITLE, badges=BADGES, keys=("Ctrl", "Ctrl"), tilt=None, seed=1,
          github=REPOSITORY):
    """The first image of a post. Upright images: the name at the top left,
    the title centred under it, the badges in a row, GitHub's mark with the
    repository's name (`github`, None for none) under them, and the window
    turned below, running off the bottom; wide ones: the title, badges and
    GitHub on the left, the window on the right running off the edges."""
    size = PRESETS[preset] or PRESETS["xhs"]
    w, h = size
    pixels, dark = background(bg, size)
    window, box = lifted(window)
    ink = "#F5F6FA" if dark else "#12141A"
    lines = title.replace("\\n", "\n").split("\n")
    upright = h >= w
    u = w / 1440 if upright else h / 900
    v = u * min(1.0, 0.75 * h / w) if upright else u  # upright but less tall (square): tighter down the page

    corner = (84 * u, 80 * u) if upright else (64 * u, 60 * u)
    pixels = put(pixels, mark((96 if upright else 80) * u, dark), corner)
    made = maker((96 if upright else 80) * u, dark)
    pixels = put(pixels, made, (w - corner[0] - made[1].shape[1], corner[1]))

    # The title, its last line in the gradient with a sparkle at its end.
    size_t = 150 * v if upright else 104 * u
    pitch = size_t * 1.3
    font = face(BOLD, size_t)
    pieces = [text_piece(line, font, GRADIENT[dark] if i == len(lines) - 1 else ink) for i, line in enumerate(lines)]
    if not upright:
        widest = max(p[1].shape[1] for p in pieces)
        if widest > 0.4 * w:  # keep it, and the sparkle after it, clear of the window
            font = face(BOLD, size_t * 0.4 * w / widest)
            pitch *= 0.4 * w / widest
            pieces = [text_piece(line, font, GRADIENT[dark] if i == len(lines) - 1 else ink)
                      for i, line in enumerate(lines)]
    y = 240 * v if upright else 200 * u
    x_text = 96 * u
    last = None
    for p in pieces:
        x = (w - p[1].shape[1]) / 2 if upright else x_text
        pixels = put(pixels, p, (x - 2, y - 2))
        last = (x, y, x + p[1].shape[1] - 4, y + p[1].shape[0] - 4)
        y += pitch
    star = sparkle(0.24 * font.size)
    pixels = put(pixels, star, (last[2] + 0.16 * font.size - star[1].shape[1] / 2,
                                last[1] - 0.06 * font.size - star[1].shape[0] / 2))

    # The badges, in a row as wide as there is room for (wide images: left of
    # the window), smaller only if they don't fit.
    badge_h = round(205 * v if upright else 150 * u)
    leaves = ("#FFE3A6", "#F4BE62") if dark else ("#FFBE6A", "#F59A45")  # warm light gold, warm amber
    space = 80 * u if upright else 36 * u
    room = 0.92 * w if upright else 0.43 * w - (x_text - 0.12 * badge_h)
    made = [badge(*(b.split("|", 1) + [""])[:2], badge_h, ink, leaves) for b in badges]
    total = sum(m[1].shape[1] for m in made) + space * (len(made) - 1)
    if made and total > room:
        badge_h = round(badge_h * room / total)
        made = [badge(*(b.split("|", 1) + [""])[:2], badge_h, ink, leaves) for b in badges]
        total = sum(m[1].shape[1] for m in made) + space * (len(made) - 1)
    top = last[3] + (90 * v if upright else 64 * u)
    if made:
        x = (w - total) / 2 if upright else x_text - 0.12 * badge_h
        for m in made:
            pixels = put(pixels, m, (x, top))
            x += m[1].shape[1] + space
        inked = np.nonzero(np.max([m[1].max(axis=1) for m in made], axis=0) > 0.1)[0]
        seen = top + inked.max() + 1  # where the laurels end, as one sees it
        top += badge_h
    else:
        seen = top

    # GitHub's mark and the name on a pill: upright, in the middle of the gap
    # between the badges and the window (placed below); wide, under the badges.
    chip_h = round(88 * v if upright else 72 * u)
    chip = github_chip(github, chip_h, dark) if github else None
    if chip and not upright:
        top += 36 * u
        pixels = put(pixels, chip, (x_text, top), 0.12 * chip_h, dark)
        top += chip_h

    # The window, upright unless told otherwise, its top a little under the
    # badges (or under the pill), running off the bottom (and in wide images
    # off the right).
    width = 1240 * u if upright else 0.6 * w
    if tilt:
        piece, centre, corners = tilted(window, box, width, tilt, focal=1.5 * max(w, h))
    else:
        piece, centre = flat(window, box, width)
        half = (width / 2, width * (box[3] - box[1]) / (box[2] - box[0]) / 2)
        corners = [(-half[0], -half[1]), (half[0], -half[1]), (half[0], half[1]), (-half[0], half[1])]
    if upright:
        middle = (w / 2, seen + (300 if chip else 190) * v - min(c[1] for c in corners))
        if chip:  # centred between the laurels' ends and the window's top edge where the pill is
            edge = middle[1] + (corners[0][1] + corners[1][1]) / 2
            pixels = put(pixels, chip, ((w - chip[1].shape[1]) / 2, (seen + edge - chip_h) / 2), 0.12 * chip_h,
                         dark)
    else:  # its left edge clear of the title
        middle = (0.47 * w - min(c[0] for c in corners), 0.3 * h - min(c[1] for c in corners))
    pixels = put(pixels, piece, (middle[0] - centre[0], middle[1] - centre[1]))  # no shadow under the program

    # The keys, on the window's top edge by its right corner.
    corner = (min(middle[0] + corners[1][0], w - 0.03 * w), middle[1] + corners[1][1])  # on the image
    key_h = round(120 * u if upright else 96 * u)
    for i, key in enumerate(keys):
        piece = turned(keycap(key, key_h, False), (-8, 5)[i % 2])
        x = corner[0] - (0.75 + 1.4 * i) * key_h - piece[1].shape[1] / 2
        y = corner[1] - (0.42 + 0.22 * i) * key_h - piece[1].shape[0] / 2
        pixels = put(pixels, piece, (x, y), 0.3 * key_h, dark)

    return Image.fromarray(np.clip(np.round(grain(pixels, seed)), 0, 255).astype(np.uint8))


# Where things are on the full-screen shot taken with the launcher opened from
# the taskbar box (2560x1600, physical px): the launcher, the taskbar's top and
# bottom, the part of the taskbar taken as it is (Start and the box), the box.
# A new screen shot needs these measured again.
TASKBAR = {"frame": (18, 617, 1158, 1510), "bar": (1528, 1600), "real": (200, 658), "box": (220, 1540, 580, 1588)}


def taskbar_page(window, screen, title, subtitle, preset="xhs", bg="letters", seed=7, places=TASKBAR):
    """The page of a post on the taskbar box: the launcher (a matte) above the
    real taskbar strip from the full-screen shot of the same moment, both at
    one scale, as on the screen, with a warm ring of light round the box."""
    frame, (bar_top, bar_bottom), real_x, box_on_screen = places["frame"], places["bar"], places["real"], places["box"]
    window, box = lifted(window)
    screen = np.asarray(screen.convert("RGB")).astype(np.float32)
    size = PRESETS[preset]
    w, h = size
    em = 0.075 * min(w, 1.1 * h)
    pixels, dark = background(bg, size)

    # One scale for everything: the launcher about as wide as on the other pages,
    # centred; the taskbar along the bottom edge, the launcher its gap above it.
    block = title_block(Image.new("RGB", size), 0, title, subtitle, 0, 0, em)
    need = 0.115 * min(size) + block + 0.07 * h  # the top row, the title, a gap round it
    s = min(0.86 * w / (frame[2] - frame[0]),  # as wide as on the other pages, or as tall as fits
            (h - need) / (bar_bottom - frame[1] + 40))
    x0 = (frame[0] + frame[2]) / 2 - w / 2 / s  # screen x at the page's left edge
    bar_h = round((bar_bottom - bar_top) * s)
    bar_y = h - bar_h

    strip = screen[bar_top:bar_bottom]
    plain = strip[:, 60:200].mean(axis=1, keepdims=True)  # empty taskbar, row by row
    band = np.repeat(plain, strip.shape[1], axis=1)
    real = strip[:, real_x[0]:real_x[1]]
    fade = np.ones(real.shape[1], np.float32)
    fade[:16] = np.linspace(0, 1, 16)
    fade[-6:] = np.linspace(1, 0, 6)
    band[:, real_x[0]:real_x[1]] = band[:, real_x[0]:real_x[1]] * (1 - fade[None, :, None]) \
        + real * fade[None, :, None]
    # The band spans the page: screen x from x0 to x0 + w / s, plain where the screen has no taskbar of ours.
    left = max(0, int(np.floor(x0)))
    right = int(np.ceil(x0 + w / s)) + 2
    wide = np.repeat(plain, right - min(0, int(np.floor(x0))), axis=1)
    start = -min(0, int(np.floor(x0)))
    wide[:, start + left:start + min(right, band.shape[1])] = band[:, left:min(right, band.shape[1])]
    img = Image.fromarray(np.clip(wide, 0, 255).astype(np.uint8))
    img = img.resize((round(img.width * s), bar_h), Image.LANCZOS)
    shift = round((min(0, int(np.floor(x0))) - x0) * s)
    bar = np.asarray(img).astype(np.float32)
    pixels[bar_y:h] = bar[:, -shift:-shift + w] if shift < 0 else bar[:, :w]

    # The launcher, at the same scale and in the same place relative to the bar.
    piece, _ = flat(window, box, (box[2] - box[0]) * s * (frame[2] - frame[0]) / (box[2] - box[0]))
    body_left = (frame[0] - x0) * s
    body_top = bar_y - (bar_top - frame[1]) * s
    scale = s * (frame[2] - frame[0]) / (box[2] - box[0])
    pixels = put(pixels, piece, (body_left - box[0] * scale, body_top - box[1] * scale))

    # A warm ring of light round the box on the taskbar, so the eye finds it.
    bx = [(box_on_screen[0] - x0) * s, bar_y + (box_on_screen[1] - bar_top) * s,
          (box_on_screen[2] - x0) * s, bar_y + (box_on_screen[3] - bar_top) * s]

    def grow(d):
        return bx[0] - d, bx[1] - d, bx[2] + d, bx[3] + d

    radius = (bx[3] - bx[1]) / 2
    outer = rounded(size, grow(9), radius + 9)
    ring = outer - rounded(size, grow(5.5), radius + 5.5)
    glow = np.clip(smooth(outer, 16) * 0.55 - rounded(size, grow(0), radius), 0, 1)
    glow[:bar_y] = 0  # on the taskbar only
    for amount, colour in ((glow, "#FF8A4C"), (ring, "#FF6A5C")):
        pixels = pixels * (1 - amount[..., None]) + rgb(colour) * amount[..., None]

    pixels = put(pixels, mark(0.06 * min(size), dark), (0.06 * min(size), 0.055 * min(size)))
    made = maker(0.06 * min(size), dark)
    pixels = put(pixels, made, (size[0] - 0.06 * min(size) - made[1].shape[1], 0.055 * min(size)))
    pixels = grain(pixels, seed)
    canvas = Image.fromarray(np.clip(np.round(pixels), 0, 255).astype(np.uint8))
    ink, soft = ("#F5F5F7", "#C9CBD3") if dark else ("#1B1B1F", "#4A4D57")
    # The title in the middle of the room between the top row and the launcher.
    room_top, room_bottom = 0.055 * min(size) + 0.06 * min(size), body_top - box[1] * scale
    title_block(canvas, round((room_top + room_bottom - block) / 2), title, subtitle, ink, soft, em, GRADIENT[dark])
    return canvas


def sheet(window, preset):
    """The window composed on every background, in a grid with the names."""
    tiles = [(name, compose(window, preset, name).convert("RGB")) for name in NAMES]
    tile_w = 520
    tile_h = round(tiles[0][1].height * tile_w / tiles[0][1].width)
    columns = 4
    rows = (len(tiles) + columns - 1) // columns
    label = 40
    out = Image.new("RGB", (columns * (tile_w + 20) + 20, rows * (tile_h + label + 20) + 20), "#FFFFFF")
    draw = ImageDraw.Draw(out)
    font = face(REGULAR, 26)
    for i, (name, tile) in enumerate(tiles):
        x, y = 20 + (i % columns) * (tile_w + 20), 20 + (i // columns) * (tile_h + label + 20)
        out.paste(tile.resize((tile_w, tile_h), Image.LANCZOS), (x, y))
        draw.text((x, y + tile_h + 6), name, font=font, fill="#333333")
    return out
