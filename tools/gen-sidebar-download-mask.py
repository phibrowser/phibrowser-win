#!/usr/bin/env python3
# Copyright 2026 Phinomenon Inc.
# Licensed under the Apache License, Version 2.0; see the LICENSE file.
"""Rasterizes the sidebar's Download icon (Figma node 9044:27222) into the
48x48 alpha mask phi_mac_icons.cc embeds as kDownloadAlpha.

The artwork is three stroked paths on a 24x24 viewBox, 1.2 wide with round
caps. No SVG renderer is assumed: the paths are transcribed below (lines
and cubic Beziers only), flattened, stroked as round-capped segments at 16x
oversampling with Pillow, and area-averaged down to 48 px.

Usage: tools/gen-sidebar-download-mask.py   (prints the C array body)
"""

from PIL import Image, ImageDraw

VIEWBOX = 24.0
MASK = 48
OVERSAMPLE = 16
STROKE = 1.2

# Each path: a start point, then ("L", x, y) or ("C", x1, y1, x2, y2, x, y).
PATHS = [
    [(8.60039, 12.068), ("L", 11.1544, 14.622),
     ("C", 11.5775, 15.0451, 12.2634, 15.0451, 12.6865, 14.622),
     ("L", 15.2405, 12.068)],
    [(11.9227, 14.1667), ("L", 11.9227, 5.5)],
    [(6.08333, 16.3334), ("L", 6.08333, 17.4167),
     ("C", 6.08333, 18.015, 6.56836, 18.5, 7.16667, 18.5),
     ("L", 16.8333, 18.5),
     ("C", 17.4316, 18.5, 17.9167, 18.015, 17.9167, 17.4167),
     ("L", 17.9167, 16.3334)],
]


def flatten(path):
    points = [path[0]]
    for seg in path[1:]:
        x0, y0 = points[-1]
        if seg[0] == "L":
            points.append((seg[1], seg[2]))
            continue
        _, x1, y1, x2, y2, x3, y3 = seg
        for i in range(1, 33):
            t = i / 32
            u = 1 - t
            points.append((
                u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 +
                t * t * t * x3,
                u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 +
                t * t * t * y3))
    return points


def main():
    size = MASK * OVERSAMPLE
    scale = size / VIEWBOX
    width = STROKE * scale
    image = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(image)
    for path in PATHS:
        points = [(x * scale, y * scale) for x, y in flatten(path)]
        draw.line(points, fill=255, width=round(width))
        # Round caps and joins: a disc at every vertex.
        r = width / 2
        for x, y in points:
            draw.ellipse((x - r, y - r, x + r, y + r), fill=255)
    mask = image.resize((MASK, MASK), Image.BOX)
    values = list(mask.tobytes())
    for row in range(0, len(values), 16):
        print("    " + ", ".join(str(v) for v in values[row:row + 16]) + ",")


if __name__ == "__main__":
    main()
