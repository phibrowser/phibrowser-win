#!/usr/bin/env python3
"""Rasterize the Mac's bookmark-folder icon SVGs into client mask tables.

The Mac draws these as template artwork (Lottie for the expand/collapse
animation, the SVG imageset for static rendering); the Windows client has no
Lottie runtime, so it ports the STATIC shapes and tints them at draw time,
exactly as phi_icon_masks.cc does for the Phi icon catalog.

Output is phibrowser-win/client/phi_folder_icon_masks.cc: 48x48 alpha masks,
run-length packed as (count, value) byte pairs, same format the catalog masks
use so PhiTintedIconBgra can consume them unchanged.

    python3 tools/gen-folder-icon-masks.py [--mac /path/to/phibrowser-mac]

Only the light (black-on-transparent) SVG of each imageset is read — the dark
variants are the same shapes in white, and the client re-tints anyway.
"""

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

MASK = 48          # Output mask, px square.
SS = 4             # Supersampling factor while filling.
CURVE_STEPS = 24   # Flattening resolution for a cubic/quadratic segment.

# The Mac's BookmarkFolderIcon.resourceMap order, with the SVG each case's
# pickerAssetName resolves to inside its imageset.
ICONS = [
    ("default", "default"),
    ("github", "github"),
    ("discord", "discord"),
    ("x", "x"),
    ("reddit", "reddit"),
    ("facebook", "facebook"),
    ("emoji-happy", "emoji-happy"),
    ("emoji-sad", "emoji-sad"),
    ("key", "key"),
    ("download", "download"),
    ("thumb-up", "thumb-up"),
    ("chart-bar", "chart-bar"),
    ("location-marker", "location-marker"),
    ("currency-dollar", "currency-dollar"),
    ("shield-check", "shield-check"),
    ("heart", "heart"),
    ("bookmark", "bookmark"),
    ("refresh", "refresh"),
    ("chat", "chat"),
    ("fire", "fire"),
    ("puzzle", "puzzle"),
    ("variable", "variable"),
    ("save", "save"),
    ("backspace", "backspace"),
]

# The plain folder the catalog's "default" wears — the Mac's own Lottie
# fallback art (Image(isExpanded ? .folderOpen : .folderClose)), which lives
# outside the folder-icons directory. Ids are client-internal.
FOLDERS = [
    ("folder-closed", "side-tab/folder-close.imageset/folder-close-regions.svg"),
    ("folder-open", "side-tab/folder-open.imageset/folder-regions.svg"),
]
# The same two files split into the layers the sidebar tints separately —
# the Mac's BookmarkFolderIconPalette gives the back body, the front panel
# and the 1pt stroke each their own accent-derived colour. Per state:
# <id>-back, <id>-back-stroke, <id>-front, <id>-front-stroke, in draw order.
FOLDER_LAYERS = [
    ("back", "folder-back-body"),
    ("front", "folder-front-panel"),
]
STROKE_WIDTH = 1.0  # User units — the SVGs' stroke="#657CAA" default width.

NUM = re.compile(r'[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?')
CMD = re.compile(r'[MmZzLlHhVvCcSsQqTtAa]')


def tokenize(d):
    """(command, [numbers]) pairs, in order."""
    out = []
    i = 0
    while i < len(d):
        ch = d[i]
        if CMD.match(ch):
            j = i + 1
            k = j
            while k < len(d) and not CMD.match(d[k]):
                k += 1
            out.append((ch, [float(n) for n in NUM.findall(d[j:k])]))
            i = k
        else:
            i += 1
    return out


def flatten(d):
    """Path data -> list of closed subpaths, each a list of (x, y)."""
    subpaths, cur = [], []
    x = y = 0.0
    start_x = start_y = 0.0
    prev_c2 = prev_q = None

    def cubic(x0, y0, x1, y1, x2, y2, x3, y3):
        for s in range(1, CURVE_STEPS + 1):
            t = s / CURVE_STEPS
            u = 1.0 - t
            cur.append((
                u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3,
            ))

    def quad(x0, y0, x1, y1, x2, y2):
        cubic(x0, y0,
              x0 + 2.0 / 3.0 * (x1 - x0), y0 + 2.0 / 3.0 * (y1 - y0),
              x2 + 2.0 / 3.0 * (x1 - x2), y2 + 2.0 / 3.0 * (y1 - y2),
              x2, y2)

    def arc(x0, y0, rx, ry, rot, large, sweep, x1, y1):
        # SVG endpoint arc -> centre parameterisation -> cubic pieces.
        import math
        if rx == 0 or ry == 0 or (x0 == x1 and y0 == y1):
            cur.append((x1, y1))
            return
        rx, ry = abs(rx), abs(ry)
        phi = math.radians(rot)
        cosp, sinp = math.cos(phi), math.sin(phi)
        dx2, dy2 = (x0 - x1) / 2.0, (y0 - y1) / 2.0
        x1p = cosp * dx2 + sinp * dy2
        y1p = -sinp * dx2 + cosp * dy2
        scale = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
        if scale > 1:
            rx *= math.sqrt(scale)
            ry *= math.sqrt(scale)
        num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
        den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
        co = math.sqrt(max(0.0, num / den)) if den else 0.0
        if large == sweep:
            co = -co
        cxp = co * rx * y1p / ry
        cyp = -co * ry * x1p / rx
        cx = cosp * cxp - sinp * cyp + (x0 + x1) / 2.0
        cy = sinp * cxp + cosp * cyp + (y0 + y1) / 2.0

        def angle(ux, uy, vx, vy):
            dot = ux * vx + uy * vy
            det = ux * vy - uy * vx
            return math.atan2(det, dot)

        theta = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
        delta = angle((x1p - cxp) / rx, (y1p - cyp) / ry,
                      (-x1p - cxp) / rx, (-y1p - cyp) / ry)
        if not sweep and delta > 0:
            delta -= 2 * math.pi
        elif sweep and delta < 0:
            delta += 2 * math.pi
        steps = max(2, int(abs(delta) / (math.pi / 8)) + 1)
        for s in range(1, steps + 1):
            t = theta + delta * s / steps
            ex = math.cos(t) * rx
            ey = math.sin(t) * ry
            cur.append((cosp * ex - sinp * ey + cx, sinp * ex + cosp * ey + cy))

    for cmd, args in tokenize(d):
        rel = cmd.islower()
        c = cmd.upper()
        if c == 'M':
            for i in range(0, len(args) - 1, 2):
                nx, ny = args[i], args[i + 1]
                if rel:
                    nx, ny = x + nx, y + ny
                if i == 0:
                    if len(cur) > 2:
                        subpaths.append(cur)
                    cur = [(nx, ny)]
                    start_x, start_y = nx, ny
                else:
                    cur.append((nx, ny))
                x, y = nx, ny
            prev_c2 = prev_q = None
        elif c == 'Z':
            if len(cur) > 2:
                subpaths.append(cur)
            cur = []
            x, y = start_x, start_y
            cur.append((x, y))
            prev_c2 = prev_q = None
        elif c == 'L':
            for i in range(0, len(args) - 1, 2):
                nx, ny = args[i], args[i + 1]
                if rel:
                    nx, ny = x + nx, y + ny
                cur.append((nx, ny))
                x, y = nx, ny
            prev_c2 = prev_q = None
        elif c == 'H':
            for v in args:
                x = x + v if rel else v
                cur.append((x, y))
            prev_c2 = prev_q = None
        elif c == 'V':
            for v in args:
                y = y + v if rel else v
                cur.append((x, y))
            prev_c2 = prev_q = None
        elif c == 'C':
            for i in range(0, len(args) - 5, 6):
                a = args[i:i + 6]
                if rel:
                    a = [a[0] + x, a[1] + y, a[2] + x, a[3] + y, a[4] + x, a[5] + y]
                cubic(x, y, a[0], a[1], a[2], a[3], a[4], a[5])
                prev_c2 = (a[2], a[3])
                x, y = a[4], a[5]
            prev_q = None
        elif c == 'S':
            for i in range(0, len(args) - 3, 4):
                a = args[i:i + 4]
                if rel:
                    a = [a[0] + x, a[1] + y, a[2] + x, a[3] + y]
                c1 = (2 * x - prev_c2[0], 2 * y - prev_c2[1]) if prev_c2 else (x, y)
                cubic(x, y, c1[0], c1[1], a[0], a[1], a[2], a[3])
                prev_c2 = (a[0], a[1])
                x, y = a[2], a[3]
            prev_q = None
        elif c == 'Q':
            for i in range(0, len(args) - 3, 4):
                a = args[i:i + 4]
                if rel:
                    a = [a[0] + x, a[1] + y, a[2] + x, a[3] + y]
                quad(x, y, a[0], a[1], a[2], a[3])
                prev_q = (a[0], a[1])
                x, y = a[2], a[3]
            prev_c2 = None
        elif c == 'T':
            for i in range(0, len(args) - 1, 2):
                nx, ny = args[i], args[i + 1]
                if rel:
                    nx, ny = x + nx, y + ny
                cp = (2 * x - prev_q[0], 2 * y - prev_q[1]) if prev_q else (x, y)
                quad(x, y, cp[0], cp[1], nx, ny)
                prev_q = cp
                x, y = nx, ny
            prev_c2 = None
        elif c == 'A':
            for i in range(0, len(args) - 6, 7):
                a = args[i:i + 7]
                nx, ny = (a[5] + x, a[6] + y) if rel else (a[5], a[6])
                arc(x, y, a[0], a[1], a[2], int(a[3]) != 0, int(a[4]) != 0, nx, ny)
                x, y = nx, ny
            prev_c2 = prev_q = None
        else:
            raise SystemExit(f'unsupported path command: {cmd}')
    if len(cur) > 2:
        subpaths.append(cur)
    return subpaths


def fill(subpaths, even_odd, size, cover):
    """Scanline-fill `subpaths` into `cover` (bytearray, size*size, 0/1)."""
    edges = []
    for sp in subpaths:
        n = len(sp)
        for i in range(n):
            x0, y0 = sp[i]
            x1, y1 = sp[(i + 1) % n]
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    if not edges:
        return
    for py in range(size):
        yc = py + 0.5
        xs = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                t = (yc - y0) / (y1 - y0)
                xs.append((x0 + t * (x1 - x0), 1 if y1 > y0 else -1))
        if not xs:
            continue
        xs.sort()
        row = py * size
        if even_odd:
            for i in range(0, len(xs) - 1, 2):
                a = max(0, int(round(xs[i][0])))
                b = min(size, int(round(xs[i + 1][0])))
                for px in range(a, b):
                    cover[row + px] = 1
        else:
            wind = 0
            for i in range(len(xs) - 1):
                wind += xs[i][1]
                if wind != 0:
                    a = max(0, int(round(xs[i][0])))
                    b = min(size, int(round(xs[i + 1][0])))
                    for px in range(a, b):
                        cover[row + px] = 1


def parse_paths(path):
    """(vw, [(element, subpaths scaled to the supersampled grid)]) — every
    drawable <path> outside defs/clipPath/mask, in document order."""
    tree = ET.parse(path)
    root = tree.getroot()
    vb = root.get('viewBox')
    if vb:
        parts = [float(v) for v in vb.replace(',', ' ').split()]
        vx, vy, vw, vh = parts
    else:
        vx = vy = 0.0
        vw = float(root.get('width', 20))
        vh = float(root.get('height', 20))

    big = MASK * SS
    sx = big / vw
    sy = big / vh

    ns = '{http://www.w3.org/2000/svg}'
    skip = set()
    for container in (f'{ns}defs', f'{ns}clipPath', f'{ns}mask'):
        for group in root.iter(container):
            for el in group.iter(f'{ns}path'):
                skip.add(id(el))
    out = []
    for el in root.iter(f'{ns}path'):
        if id(el) in skip:
            continue
        d = el.get('d')
        if not d:
            continue
        subpaths = [[((px - vx) * sx, (py - vy) * sy) for px, py in sp]
                    for sp in flatten(d)]
        out.append((el, subpaths))
    return vw, out


def even_odd_of(el):
    return (el.get('fill-rule') or el.get('clip-rule') or '') == 'evenodd'


def downsample(cover):
    """Box-average the supersampled coverage down to the 48px alpha mask."""
    big = MASK * SS
    mask = bytearray(MASK * MASK)
    area = SS * SS
    for my in range(MASK):
        for mx in range(MASK):
            total = 0
            for oy in range(SS):
                base = (my * SS + oy) * big + mx * SS
                for ox in range(SS):
                    total += cover[base + ox]
            mask[my * MASK + mx] = (total * 255) // area
    return mask


def rasterize(path):
    big = MASK * SS
    cover = bytearray(big * big)
    for el, subpaths in parse_paths(path)[1]:
        if (el.get('fill') or '').lower() == 'none':
            continue
        fill(subpaths, even_odd_of(el), big, cover)
    return downsample(cover)


def ring(cover, radius):
    """The stroke of a filled shape: every supersampled pixel within `radius`
    of the fill's boundary, on either side — a stroke centred on the path,
    which is how the SVG paints it."""
    big = MASK * SS
    disk = [(dx, dy) for dy in range(-int(radius) - 1, int(radius) + 2)
            for dx in range(-int(radius) - 1, int(radius) + 2)
            if dx * dx + dy * dy <= radius * radius]
    out = bytearray(big * big)
    for y in range(big):
        row = y * big
        for x in range(big):
            if not cover[row + x]:
                continue
            edge = (x == 0 or y == 0 or x == big - 1 or y == big - 1 or
                    not cover[row + x - 1] or not cover[row + x + 1] or
                    not cover[row - big + x] or not cover[row + big + x])
            if not edge:
                continue
            for dx, dy in disk:
                px, py = x + dx, y + dy
                if 0 <= px < big and 0 <= py < big:
                    out[py * big + px] = 1
    return out


def rasterize_layers(path, prefix):
    """[(id, mask)] for the plain folder's tintable layers, in draw order."""
    big = MASK * SS
    vw, paths = parse_paths(path)
    by_id = {el.get('id'): (el, subpaths) for el, subpaths in paths}
    radius = STROKE_WIDTH / 2 * big / vw
    out = []
    for layer, path_id in FOLDER_LAYERS:
        if path_id not in by_id:
            sys.exit(f'{path}: no <path id="{path_id}">')
        el, subpaths = by_id[path_id]
        cover = bytearray(big * big)
        fill(subpaths, even_odd_of(el), big, cover)
        out.append((f'{prefix}-{layer}', downsample(cover)))
        out.append((f'{prefix}-{layer}-stroke', downsample(ring(cover, radius))))
    return out


def rle(mask):
    out = []
    i = 0
    while i < len(mask):
        v = mask[i]
        run = 1
        while i + run < len(mask) and mask[i + run] == v and run < 255:
            run += 1
        out.append((run, v))
        i += run
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mac', default=os.path.expanduser('~/Phi/phibrowser-mac'))
    ap.add_argument('--out', default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        'client', 'phi_folder_icon_masks.cc'))
    args = ap.parse_args()

    assets = os.path.join(args.mac, 'Resources', 'Assets.xcassets')
    root = os.path.join(assets, 'icons', 'folder-icons')
    sources = [(i, os.path.join(root, f'{a}.imageset', f'{a}.svg'))
               for i, a in ICONS]
    sources += [(i, os.path.join(assets, rel)) for i, rel in FOLDERS]
    blob, entries = [], []
    masks = []
    for icon_id, svg in sources:
        if not os.path.exists(svg):
            sys.exit(f'missing {svg}')
        masks.append((icon_id, rasterize(svg)))
    for icon_id, rel in FOLDERS:
        masks += rasterize_layers(os.path.join(assets, rel), icon_id)
    for icon_id, mask in masks:
        pairs = rle(mask)
        offset = len(blob)
        for run, value in pairs:
            blob.append(run)
            blob.append(value)
        entries.append((icon_id, offset, len(pairs) * 2))
        print(f'{icon_id:18} {len(pairs) * 2:6} bytes', file=sys.stderr)

    with open(args.out, 'w') as f:
        f.write('''// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

// GENERATED by tools/gen-folder-icon-masks.py from the Mac's bookmark-folder
// icon imagesets (Resources/Assets.xcassets/icons/folder-icons, light
// variants — the dark SVGs are the same shapes in white). 48x48 alpha masks,
// run-length packed as (count, value) byte pairs. Do not edit by hand.

#include "phibrowser-win/client/phi_folder_icons.h"

#include <map>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/no_destructor.h"

namespace phi_client {

namespace {

constexpr int kMaskSize = 48;

constexpr unsigned char kMaskRle[] = {
''')
        line = '    '
        for i, b in enumerate(blob):
            piece = f'{b},'
            if len(line) + len(piece) > 76:
                f.write(line.rstrip() + '\n')
                line = '    '
            line += piece + ' '
        if line.strip():
            f.write(line.rstrip() + '\n')
        f.write('''};

struct MaskEntry {
  const char* id;
  unsigned int offset;
  unsigned int length;
};

constexpr MaskEntry kMaskEntries[] = {
''')
        for icon_id, offset, length in entries:
            f.write(f'    {{"{icon_id}", {offset}, {length}}},\n')
        f.write('''};

}  // namespace

const PhiMacIcon* PhiFolderIconMask(const std::string& id) {
  // Decoded once; map nodes never move, so the PhiMacIcon views handed out
  // stay valid for the process lifetime.
  struct Tables {
    std::map<std::string, std::vector<unsigned char>> data;
    std::map<std::string, PhiMacIcon> icons;
  };
  static const base::NoDestructor<Tables> tables([] {
    const base::span<const unsigned char> rle(kMaskRle);
    Tables t;
    for (const MaskEntry& entry : kMaskEntries) {
      std::vector<unsigned char> mask;
      mask.reserve(kMaskSize * kMaskSize);
      for (unsigned int i = 0; i + 1 < entry.length; i += 2) {
        const unsigned char run = rle[entry.offset + i];
        const unsigned char value = rle[entry.offset + i + 1];
        mask.insert(mask.end(), run, value);
      }
      t.data[entry.id] = std::move(mask);
    }
    for (const auto& [icon_id, mask] : t.data) {
      t.icons[icon_id] = {kMaskSize, kMaskSize, mask.data()};
    }
    return t;
  }());
  auto it = tables->icons.find(id);
  return it == tables->icons.end() ? nullptr : &it->second;
}

}  // namespace phi_client
''')
    print(f'wrote {args.out} ({len(blob)} rle bytes)', file=sys.stderr)


if __name__ == '__main__':
    main()
