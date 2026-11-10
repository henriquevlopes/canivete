# Draws Canivete's icons: an open pocket knife (canivete) with no background, red handle.
# Tray: the blade is light for a dark taskbar and dark for a light taskbar.
# App (exe, windows, Start menu, installer): a steel blade with a dark edge, readable on any background.
#   python tools/make_icons.py  -> res/tray_dark.ico, res/tray_light.ico, res/canivete.ico, res/*_preview.png
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RED = (227, 45, 58, 255)
BLADE = {"dark": (243, 243, 243, 255), "light": (40, 40, 40, 255), "app": (214, 217, 222, 255)}
APP_EDGE = (78, 82, 90, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64]
APP_SIZES = SIZES + [96, 128, 256]
SS = 8  # supersampling


def bezier(p0, p1, p2, n=24):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in (i / n for i in range(n + 1))]


def capsule(x0, x1, r, n=24):
    pts = [(x1 + r * math.cos(a), r * math.sin(a)) for a in (-math.pi / 2 + math.pi * i / n for i in range(n + 1))]
    pts += [(x0 + r * math.cos(a), r * math.sin(a)) for a in (math.pi / 2 + math.pi * i / n for i in range(n + 1))]
    return pts


def shapes(size):
    """Knife along +x (handle left, blade right), in arbitrary units; fitted to the icon later."""
    small = size <= 20  # small sizes: slightly thicker parts and a wider gap, no rivet
    hr = 6.6 if small else 6.0  # handle half-height
    handle = capsule(-24, -2, hr)
    b0 = -2 + hr + (3.0 if small else 2.2)  # blade base, just past the handle's round end
    top, bot = -hr, (3.4 if small else 2.8)
    tip = (38.0, -hr + 1.6)
    blade = [(b0, top), (28.0, top)] + bezier((28.0, top), (34.0, top), tip)[1:]
    blade += bezier(tip, (30.0, bot), (20.0, bot))[1:] + [(b0, bot)]
    rivet = None if small else (-4.5, 0.0, 1.9)
    return handle, blade, rivet


def fit(size, groups):
    """Rotate 45 degrees (handle bottom-left, blade top-right) and scale/center into the icon."""
    a = math.radians(-45)
    c, s = math.cos(a), math.sin(a)
    rot = [[(x * c - y * s, x * s + y * c) for x, y in g] for g in groups]
    xs = [p[0] for g in rot for p in g]
    ys = [p[1] for g in rot for p in g]
    S = size * SS
    margin = S * (0.5 / 16 if size <= 20 else 1.0 / 32)
    k = (S - 2 * margin) / max(max(xs) - min(xs), max(ys) - min(ys))
    ox = S / 2 - (max(xs) + min(xs)) / 2 * k
    oy = S / 2 - (max(ys) + min(ys)) / 2 * k
    return [[(ox + x * k, oy + y * k) for x, y in g] for g in rot], k


def render(size, theme):
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    handle, blade, rivet = shapes(size)
    groups = [handle, blade] + ([[rivet[:2]]] if rivet else [])  # the rivet is inside the handle: same fit
    placed, k = fit(size, groups)
    d.polygon(placed[0], fill=RED)
    if theme == "app":
        edge = max(SS, round(S / 40))
        for pts, fill, line, w in ((placed[1], BLADE[theme], APP_EDGE, edge),
                                   (placed[0], RED, (170, 24, 36, 255), max(SS // 2, round(S / 64)))):
            d.polygon(pts, fill=fill)
            d.line(pts + pts[:2], fill=line, width=w, joint="curve")  # round joins: no seams at the tip
    else:
        d.polygon(placed[1], fill=BLADE[theme])
    if rivet:
        (cx, cy), r = placed[2][0], rivet[2] * k
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(0, 0, 0, 0))  # a hole: the taskbar shows through
    return img.resize((size, size), Image.LANCZOS)


def main():
    for theme in ("dark", "light"):
        imgs = [render(s, theme) for s in SIZES]
        imgs[-1].save(os.path.join(ROOT, "res", f"tray_{theme}.ico"), sizes=[(s, s) for s in SIZES],
                      append_images=imgs[:-1])
    # preview: each size on a dark and a light taskbar color, 4x nearest-neighbor
    W = sum(s + 8 for s in SIZES) + 8
    prev = Image.new("RGBA", (W, 2 * 72), (32, 32, 32, 255))
    ImageDraw.Draw(prev).rectangle((0, 72, W, 144), fill=(238, 238, 238, 255))
    x = 8
    for s in SIZES:
        prev.alpha_composite(render(s, "dark"), (x, 36 - s // 2))
        prev.alpha_composite(render(s, "light"), (x, 108 - s // 2))
        x += s + 8
    prev.resize((W * 4, 144 * 4), Image.NEAREST).save(os.path.join(ROOT, "res", "tray_preview.png"))

    apps = [render(s, "app") for s in APP_SIZES]
    apps[-1].save(os.path.join(ROOT, "res", "canivete.ico"), sizes=[(s, s) for s in APP_SIZES], append_images=apps[:-1])
    small = [s for s in APP_SIZES if s <= 64]
    W = sum(s + 8 for s in small) + 8 + 264
    prev = Image.new("RGBA", (W, 2 * 272), (32, 32, 32, 255))
    ImageDraw.Draw(prev).rectangle((0, 272, W, 544), fill=(250, 250, 250, 255))
    for row, y in ((0, 136), (1, 408)):
        x = 8
        for s, im in zip(APP_SIZES, apps):
            if s in small or s == 256:
                prev.alpha_composite(im, (x, y - s // 2))
                x += s + 8
    prev.resize((W * 2, 544 * 2), Image.NEAREST).save(os.path.join(ROOT, "res", "app_preview.png"))


if __name__ == "__main__":
    main()
