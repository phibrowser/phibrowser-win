#!/usr/bin/env python3
# Copyright 2026 Phinomenon Inc.
# Licensed under the Apache License, Version 2.0; see the LICENSE file.
"""Rebuilds client/resources/phi.ico, the application icon Phi.exe embeds.

Every size is resampled (Lanczos, then a light unsharp mask below 64 px) from
ONE master — the 256 px image already in phi.ico, or a PNG given with
--master. Windows picks the image nearest the size it draws and shrinks it
when there is no exact match, which blurs: desktop icons at 125 % scaling are
60 px, at 150 % 72 px, so those sizes are carried explicitly (the Windows
icon guidelines' list plus the scaled shell sizes).

Usage:
    tools/gen-app-icon.py                  # rewrite phi.ico from its 256 px
    tools/gen-app-icon.py --master big.png # rewrite from another master
"""

import argparse
import io
import os
import struct

from PIL import Image, ImageFilter

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICO = os.path.join(REPO, "client", "resources", "phi.ico")

# 16/20/24/32/40/48/64/96/256 are the shell's 100 % sizes; 30/36/60/72/80/
# 120/128 its 125 % / 150 % twins (small, medium and large desktop icons,
# the taskbar, Start, Explorer's views).
SIZES = (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 120, 128, 256)


def read_ico_master(path):
    data = open(path, "rb").read()
    count = struct.unpack("<HHH", data[:6])[2]
    best = None
    for i in range(count):
        w, h, _, _, _, _, size, offset = struct.unpack(
            "<BBBBHHII", data[6 + 16 * i:22 + 16 * i])
        w = w or 256
        if best is None or w > best[0]:
            best = (w, data[offset:offset + size])
    return Image.open(io.BytesIO(best[1])).convert("RGBA")


def resample(master, size):
    if master.size == (size, size):
        return master.copy()
    # Premultiply so the transparent corners do not bleed dark fringes.
    im = master.convert("RGBa").resize((size, size), Image.LANCZOS)
    im = im.convert("RGBA")
    if size < 64:
        rgb = im.convert("RGB").filter(
            ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
        rgb.putalpha(im.getchannel("A"))
        im = rgb
    return im


def write_ico(images, path):
    entries = []
    blobs = []
    offset = 6 + 16 * len(images)
    for im in images:
        buf = io.BytesIO()
        im.save(buf, "PNG", optimize=True)
        blob = buf.getvalue()
        w = im.width if im.width < 256 else 0
        entries.append(struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob),
                                   offset))
        blobs.append(blob)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        f.write(b"".join(entries))
        f.write(b"".join(blobs))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--master", help="a square PNG, 256 px or larger")
    args = parser.parse_args()
    master = (Image.open(args.master).convert("RGBA") if args.master
              else read_ico_master(ICO))
    if master.width < 256 or master.width != master.height:
        parser.error("the master must be square and at least 256 px")
    if master.width > 256:
        master = master.convert("RGBa").resize(
            (256, 256), Image.LANCZOS).convert("RGBA")
    write_ico([resample(master, s) for s in SIZES], ICO)
    print(f"wrote {ICO}: {', '.join(map(str, SIZES))}")


if __name__ == "__main__":
    main()
