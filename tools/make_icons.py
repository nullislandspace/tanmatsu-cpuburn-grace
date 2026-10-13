#!/usr/bin/env python3
"""Generate CPU Burn's application icons into metadata/.

The launcher puts one of these beside the app's name; metadata.json names
all three sizes and the launcher picks by how much room it has. They are
generated, so what is committed can be re-derived from the numbers in this
file instead of from a lost .xcf:

    python3 tools/make_icons.py            # metadata/icon{16,32,64}.png
    python3 tools/make_icons.py --preview  # also a magnified sheet

The subject is the thing the app loads: the ESP32-P4 seen from above, its
two cores lit as hard as the palette allows. That is the whole job in one
picture -- a chip running at 100 % on both cores -- and it is what the
screen next to it is measuring.

WHY IT IS DRAWN ON A 16-UNIT GRID. The three icons are one picture at three
scales, not three drawings that will disagree after the first edit: every
coordinate below is in sixteenths of the icon and multiplied by 1, 2 or 4 on
the way out. The larger two then get detail that has nowhere to go at 16 --
a lighter lid on the package, a pin-1 dot, heat glowing out past the
silhouette -- and the smallest gets everything it needs on its own.

WHY THERE IS NO BACKGROUND. The launcher draws these on the theme's menu
colour, which is #EEEAEE in the light theme and #000000 in the dark one. A
full-bleed backdrop would have to be one of those two and would be wrong on
the other, so the icon carries its own contrast instead: a graphite package
that reads on both, a lighter edge that draws the silhouette on black, and
two cores bright enough to be the first thing you see either way.
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

SIZES = (16, 32, 64)
OUT = Path(__file__).resolve().parent.parent / "metadata"

PACKAGE = (62, 67, 76)        # the body of a chip: dark, but not black
PACKAGE_EDGE = (140, 148, 162)  # the silhouette, which has to survive a black menu
PACKAGE_LID = (84, 90, 102)   # the top face, catching a little light
PIN = (198, 203, 212)         # the leads

HEAT_HOT = (255, 233, 160)    # the centre of a core that is doing everything
HEAT_MID = (255, 150, 45)
HEAT_COLD = (168, 40, 18)     # the core's edge, where the die meets the package

# The package and its leads, in grid units. Half-unit coordinates are chosen
# so that at every one of the three sizes the package, the gap between the
# cores and the leads all land on whole pixels and stay symmetric.
BODY = (2.5, 2.5, 13.5, 13.5)      # x0, y0, x1, y1
PIN_SLOTS = (3.0, 6.0, 9.0, 12.0)  # one end of each of the four leads per side
PIN_WIDTH = 2.0
PIN_LENGTH = 1.5

# The two cores, side by side with a unit of package between them. They are
# taller than they are wide so that at 16 -- four pixels by six -- each one
# still has a hot middle, a warm edge and a cool rim, which is the whole
# point of the picture.
CORES = ((3.5, 5.0, 7.5, 11.0), (8.5, 5.0, 12.5, 11.0))


def _edge(value):
    """Round half up. Python's round() rounds half to even, which would put
    both cores on the same side of the gap at 16 and turn the pair into one
    wide block."""
    return int(np.floor(value + 0.5))


def _rect(img, s, x0, y0, x1, y1, rgb):
    """Fill a rectangle given in grid units. Coordinates may be halves; at
    s == 1 they land on whole pixels, which is the point of a 16-unit grid
    for a 16-pixel icon."""
    y = slice(_edge(y0 * s), _edge(y1 * s))
    x = slice(_edge(x0 * s), _edge(x1 * s))
    img[y, x, :3] = rgb
    img[y, x, 3] = 255


def _heat(size, s, x0, y0, x1, y1):
    """One core, hot in the middle and cooling to its edges: the gradient is
    what says 100 % rather than a flat red square, which could be anything
    from a warning to a logo.

    The distance is normalised against the furthest pixel centre in the core,
    not against its half width: pixel centres never reach the corners of the
    rect, and normalising by the rect would leave the corners mid-orange at
    16 and the middle barely warm at 64. This way the ramp always spans the
    whole palette, at every size."""
    px0, py0, px1, py1 = (_edge(v * s) for v in (x0, y0, x1, y1))
    w, h = px1 - px0, py1 - py0
    cx, cy = (px0 + px1 - 1) / 2.0, (py0 + py1 - 1) / 2.0

    row, col = np.mgrid[py0:py1, px0:px1]
    dist = np.hypot(col - cx, row - cy)
    t = dist / dist.max()

    hot, mid = np.array(HEAT_HOT, float), np.array(HEAT_MID, float)
    cold = np.array(HEAT_COLD, float)
    k1 = np.clip(t / 0.70, 0.0, 1.0)
    k2 = np.clip((t - 0.70) / 0.30, 0.0, 1.0)
    rgb = hot + (mid - hot) * k1[:, :, None]
    rgb = rgb * (1.0 - k2[:, :, None]) + cold * k2[:, :, None]

    out = np.zeros((h, w, 4), np.uint8)
    out[:, :, :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    out[:, :, 3] = 255
    return out, (py0, px0)


def _glow(img, s):
    """Heat radiating out past the package. Only above 16: at one pixel per
    grid unit it is a smudge on the leads rather than a glow, and the small
    icon is better off being crisp."""
    for x0, y0, x1, y1 in CORES:
        cx = (x0 + x1) / 2.0 * s
        cy = (y0 + y1) / 2.0 * s
        reach = 5.5 * s
        ys, xs = np.mgrid[0:img.shape[0], 0:img.shape[1]]
        d = np.hypot(xs - cx, ys - cy)
        strength = np.clip(1.0 - d / reach, 0.0, 1.0)
        free = (img[:, :, 3] == 0) & (d < reach)
        alpha = strength * 215
        img[free, 0] = np.clip(img[free, 0] + HEAT_MID[0] * alpha[free] / 255.0, 0, 255)
        img[free, 1] = np.clip(img[free, 1] + HEAT_MID[1] * alpha[free] / 255.0, 0, 255)
        img[free, 2] = np.clip(img[free, 2] + HEAT_MID[2] * alpha[free] / 255.0, 0, 255)
        img[free, 3] = np.clip(alpha[free], 0, 255)


def icon(size):
    s = size // 16
    img = np.zeros((size, size, 4), np.uint8)

    # Leads first, so the package sits on top of their inner ends.
    for slot in PIN_SLOTS:
        _rect(img, s, slot, BODY[1] - PIN_LENGTH, slot + PIN_WIDTH, BODY[1], PIN)          # top
        _rect(img, s, slot, BODY[3], slot + PIN_WIDTH, BODY[3] + PIN_LENGTH, PIN)          # bottom
        _rect(img, s, BODY[0] - PIN_LENGTH, slot, BODY[0], slot + PIN_WIDTH, PIN)          # left
        _rect(img, s, BODY[2], slot, BODY[2] + PIN_LENGTH, slot + PIN_WIDTH, PIN)          # right

    # The package, with a lighter edge so the shape reads on a black menu.
    _rect(img, s, BODY[0], BODY[1], BODY[2], BODY[3], PACKAGE)
    for x0, y0, x1, y1 in ((BODY[0], BODY[1], BODY[2], BODY[1] + 1), (BODY[0], BODY[3] - 1, BODY[2], BODY[3]),
                           (BODY[0], BODY[1], BODY[0] + 1, BODY[3]), (BODY[2] - 1, BODY[1], BODY[2], BODY[3])):
        _rect(img, s, x0, y0, x1, y1, PACKAGE_EDGE)

    if s >= 2:
        # A lid, and the dot that marks pin 1: package detail that only has
        # room above 16, and that makes the square read as a chip rather
        # than as a box.
        _rect(img, s, BODY[0] + 1, BODY[1] + 1, BODY[2] - 1, BODY[1] + 2.5, PACKAGE_LID)
        _rect(img, s, BODY[0] + 1, BODY[1] + 1, BODY[0] + 2.5, BODY[1] + 2.5, PACKAGE_EDGE)

    for x0, y0, x1, y1 in CORES:
        heat, (py, px) = _heat(size, s, x0, y0, x1, y1)
        img[py:py + heat.shape[0], px:px + heat.shape[1]] = heat

    if s >= 2:
        _glow(img, s)

    return img


def main():
    icons = []
    for size in SIZES:
        img = icon(size)
        path = OUT / f"icon{size}.png"
        Image.fromarray(img, "RGBA").save(path, optimize=True)
        icons.append(img)
        print(f"wrote {path}")

    if "--preview" in sys.argv:
        pad = max(SIZES)
        row = [np.pad(i, ((0, pad - i.shape[0]), (0, 4), (0, 0))) for i in icons]
        sheet = np.concatenate(row, axis=1)
        prev = Image.fromarray(sheet, "RGBA").resize(
            (sheet.shape[1] * 6, sheet.shape[0] * 6), Image.NEAREST)
        path = OUT.parent / "build" / "icons_preview.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        prev.save(path)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
