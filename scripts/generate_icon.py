#!/usr/bin/env python3
"""Generate app/app.ico for MediaAnalyse.

Pure standard-library implementation (no Pillow/numpy): draws a dark PRISM
rounded tile with an amber play glyph and three analysis bars, then writes a
multi-resolution Windows ICO (16/32/48/64/128/256, PNG-compressed entries).

Regenerate with:  python scripts/generate_icon.py
"""

import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "app", "app.ico")
SIZES = (16, 24, 32, 48, 64, 128, 256)

# PRISM tokens (app/qml/Theme.qml)
BG_TOP = (0x23, 0x28, 0x30)
BG_BOT = (0x10, 0x13, 0x17)
BORDER = (0x4C, 0x56, 0x63)
AMBER = (0xE0, 0xA4, 0x4A)
BLUE = (0x82, 0xA9, 0xCC)
GREEN = (0x86, 0xB4, 0x9A)


def clamp(v, lo=0.0, hi=1.0):
    return lo if v < lo else (hi if v > hi else v)


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def overlay(dst, color, alpha):
    """Alpha-composite an opaque color onto an RGB pixel."""
    a = clamp(alpha)
    if a <= 0.0:
        return dst
    return mix(dst, color, a)


def sd_round_rect(px, py, cx, cy, hw, hh, r):
    """Signed distance to a rounded rectangle (negative inside)."""
    dx = abs(px - cx) - (hw - r)
    dy = abs(py - cy) - (hh - r)
    return math.hypot(max(dx, 0.0), max(dy, 0.0)) + min(max(dx, dy), 0.0) - r


def cross(ax, ay, bx, by, px, py):
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax)


def _segment_distance(px, py, a, b):
    abx, aby = b[0] - a[0], b[1] - a[1]
    apx, apy = px - a[0], py - a[1]
    den = abx * abx + aby * aby
    t = clamp((apx * abx + apy * aby) / den) if den > 0 else 0.0
    return math.hypot(apx - t * abx, apy - t * aby)


def sd_triangle(px, py, a, b, c):
    """Signed distance to a triangle (negative inside).

    Outside must use distance to the *segments*, not to the infinite lines,
    otherwise the edge extensions leak thin anti-aliased streaks.
    """
    c1 = cross(a[0], a[1], b[0], b[1], px, py)
    c2 = cross(b[0], b[1], c[0], c[1], px, py)
    c3 = cross(c[0], c[1], a[0], a[1], px, py)
    inside = (c1 >= 0 and c2 >= 0 and c3 >= 0) or (c1 <= 0 and c2 <= 0 and c3 <= 0)
    edges = ((a, b, c1), (b, c, c2), (c, a, c3))
    if inside:
        dmin = float("inf")
        for p, q, cr in edges:
            length = math.hypot(q[0] - p[0], q[1] - p[1])
            if length > 0:
                dmin = min(dmin, abs(cr) / length)
        return -dmin
    return min(_segment_distance(px, py, p, q) for p, q, _ in edges)


def sd_box(px, py, x0, y0, x1, y1):
    dx = max(x0 - px, px - x1)
    dy = max(y0 - py, py - y1)
    if dx < 0 and dy < 0:
        return max(dx, dy)
    return math.hypot(max(dx, 0.0), max(dy, 0.0))


def coverage(sdf):
    """1-pixel anti-aliasing from a signed distance in pixel units."""
    return clamp(0.5 - sdf)


def render(size):
    """Return RGBA bytes for one icon size."""
    s = float(size)
    cx = cy = s / 2.0
    half = s * 0.46
    radius = s * 0.22
    border_w = max(1.0, s / 32.0)
    detailed = size >= 48

    # Geometry in normalized 0..1 coordinates, scaled by size.
    if detailed:
        tri = ((0.32, 0.26), (0.60, 0.50), (0.32, 0.74))
    else:
        tri = ((0.30, 0.22), (0.68, 0.50), (0.30, 0.78))
    tri_px = tuple((x * s, y * s) for x, y in tri)

    if detailed:
        bars = (
            (0.66, 0.70, 0.40, BLUE),
            (0.745, 0.785, 0.56, GREEN),
            (0.83, 0.87, 0.46, AMBER),
        )
    else:
        bars = ()

    rows = bytearray()
    for y in range(size):
        py = y + 0.5
        for x in range(size):
            px = x + 0.5

            d = sd_round_rect(px, py, cx, cy, half, half, radius)
            a_bg = coverage(d)
            if a_bg <= 0.0:
                rows += bytes((0, 0, 0, 0))
                continue

            t = clamp(py / s)
            color = mix(BG_TOP, BG_BOT, t)
            # Border ring: inside edge of the rounded tile.
            a_border = clamp((d + border_w) / border_w) * a_bg
            color = overlay(color, BORDER, a_border * 0.85)

            # Play glyph.
            dt = sd_triangle(px, py, *tri_px)
            color = overlay(color, AMBER, coverage(dt) * a_bg)

            # Analysis bars, sitting on a common baseline.
            for x0n, x1n, hn, bar_color in bars:
                b = sd_box(px, py, x0n * s, (0.74 - hn) * s,
                           x1n * s, 0.74 * s)
                color = overlay(color, bar_color, coverage(b) * a_bg)

            rows += bytes((color[0], color[1], color[2], int(round(a_bg * 255))))

    return bytes(rows)


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def encode_png(size, rgba):
    stride = size * 4
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter: None
        raw += rgba[y * stride:(y + 1) * stride]
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


def write_ico(path):
    images = []
    for size in SIZES:
        data = encode_png(size, render(size))
        images.append((size, data))
        print("  rendered %3dx%-3d  %5d bytes" % (size, size, len(data)))

    count = len(images)
    header = struct.pack("<HHH", 0, 1, count)
    offset = 6 + 16 * count
    entries = bytearray()
    blob = bytearray()
    for size, data in images:
        entries += struct.pack(
            "<BBBBHHII",
            size % 256, size % 256, 0, 0, 1, 32, len(data), offset,
        )
        offset += len(data)
        blob += data
    with open(path, "wb") as f:
        f.write(header + bytes(entries) + bytes(blob))
    print("wrote", path, os.path.getsize(path), "bytes")


if __name__ == "__main__":
    write_ico(OUT)
