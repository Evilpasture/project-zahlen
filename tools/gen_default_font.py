#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate resources/fonts/DefaultFont.zfont from the checked-in Font8x8 data.

This is the offline half of the engine's TTF decoupling: core never rasterises
fonts at runtime, it only decodes cooked 'FNT0' containers (see
include/Zahlen/gui/FontLoader.hpp and the CookedFontHeader in
include/Zahlen/CreativeWorksManager.hpp). The zero-asset standalone build needs
one such container embedded in the engine (src/gui/FontLoader.cpp #embeds the
file this script writes), and its source data is the public-domain Font8x8
bitmap table -- not a TTF, so no outline-font parser is involved anywhere in
core or in its build.

The generator re-bakes those bitmaps as a true signed distance field (the same
onedge/pixel-distance convention `zcook font` and stb_truetype use), so the
built-in font renders through the shader's SDF path instead of shipping raw
binary coverage.

Regenerate with:

    python3 tools/gen_default_font.py

The output is deterministic; commit it next to the generator when it changes.
"""

from __future__ import annotations

import argparse
import math
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HEADER = ROOT / "src" / "engine" / "Font8x8.hpp"
DEFAULT_OUT = ROOT / "resources" / "fonts" / "DefaultFont.zfont"

# --- Cooked font constants (must match CookedFontHeader / zcook font) --------
FNT0_MAGIC = 0x30544E46  # 'FNT0' little-endian
FNT0_VERSION = 1
FLAG_SDF = 1

# --- Bake parameters --------------------------------------------------------
# Metrics reproduce the historical built-in font exactly (32px nominal size,
# 28px baseline, 36px line height, 18px advance): the layout numbers moved
# from hard-coded constants into the asset, not from one value to another.
FONT_SIZE = 32.0
BASELINE = 28.0
LINE_HEIGHT = 36.0
XADVANCE = 18.0

FIRST_CODEPOINT = 32
GLYPH_COUNT = 96  # printable ASCII 32..127

PIXEL_SCALE = 2          # 8x8 bitmap -> 16x16 content block
SDF_PADDING = 4          # content block -> 24x24 signed distance cell
CELL = 8 * PIXEL_SCALE + 2 * SDF_PADDING  # 24
GUTTER = 2
ONEDGE = 128
PIXEL_DIST_SCALE = 128.0 / SDF_PADDING  # stbtt convention

# Shelf layout: 9 cells of 26px per row fit a 256-wide atlas; 11 rows of 26px
# fit 288px of height (2px margin + rows + 2px bottom clearance).
ATLAS_W = 256
ATLAS_H = 288
CELLS_PER_ROW = 9


def parse_font8x8(header: Path) -> list[list[int]]:
    """Extract the 128 glyph bitmaps (8 row-bytes each) from Font8x8.hpp."""
    text = header.read_text(encoding="utf-8")
    match = re.search(r"Font8x8_Basic\[128\]\[8\]\s*=\s*\{(.*)\n\};", text, re.DOTALL)
    if not match:
        raise SystemExit(f"could not find Font8x8_Basic in {header}")
    body = re.sub(r"//[^\n]*", "", match.group(1))  # drop line comments

    glyphs: list[list[int]] = []
    for brace in re.findall(r"\{([^{}]*)\}", body):
        values = [int(tok.strip(), 0) for tok in brace.split(",") if tok.strip()]
        if len(values) == 1:      # `{0}` C aggregate shorthand: all rows zero
            values *= 8
        if len(values) != 8:
            raise SystemExit(f"malformed Font8x8 row group: {{{brace}}}")
        glyphs.append(values)
    if len(glyphs) != 128:
        raise SystemExit(f"expected 128 Font8x8 glyph rows, found {len(glyphs)}")
    return glyphs


def content_block(rows: list[int]) -> list[list[int]]:
    """8x8 bitmap (MSB = leftmost pixel, as stored in Font8x8.hpp) upsampled."""
    size = 8 * PIXEL_SCALE
    block = [[0] * size for _ in range(size)]
    for r, row_bits in enumerate(rows):
        for c in range(8):
            # Font8x8_Basic stores MSB as leftmost: 0x80 = leftmost column
            if (row_bits >> (7 - c)) & 1:
                for dy in range(PIXEL_SCALE):
                    for dx in range(PIXEL_SCALE):
                        block[r * PIXEL_SCALE + dy][c * PIXEL_SCALE + dx] = 1
    return block


def sdf_cell(block: list[list[int]]) -> list[int]:
    """Brute-force signed distance field of a 24x24 padded cell, row-major.

    Positive inside the glyph, zero crossing on the content boundary: coverage
    is 128 on the edge and ramps PIXEL_DIST_SCALE per pixel either way, the
    convention stbtt_GetCodepointSDF bakes with and the shader's 0.5 isolevel
    thresholds.
    """
    size = 8 * PIXEL_SCALE
    dim = size + 2 * SDF_PADDING
    field = [[0] * dim for _ in range(dim)]

    set_pixels = [
        (r, c) for r in range(size) for c in range(size) if block[r][c]
    ]
    clear_pixels = [
        (r, c) for r in range(size) for c in range(size) if not block[r][c]
    ]

    for y in range(dim):
        for x in range(dim):
            by = y - SDF_PADDING
            bx = x - SDF_PADDING
            inside = 0 <= by < size and 0 <= bx < size and block[by][bx]

            if inside:
                if not clear_pixels:
                    val = 255
                else:
                    dist = min(math.hypot(y - SDF_PADDING - q, x - SDF_PADDING - p) for q, p in clear_pixels)
                    val = ONEDGE + (dist - 0.5) * PIXEL_DIST_SCALE
            else:
                if not set_pixels:
                    val = 0
                else:
                    dist = min(math.hypot(y - SDF_PADDING - q, x - SDF_PADDING - p) for q, p in set_pixels)
                    val = ONEDGE - (dist - 0.5) * PIXEL_DIST_SCALE

            field[y][x] = max(0, min(255, int(round(val))))
    return [px for row in field for px in row]


def bake(header_path: Path) -> tuple[bytes, list[dict]]:
    glyphs8 = parse_font8x8(header_path)
    coverage = bytearray(ATLAS_W * ATLAS_H)
    records = []

    cur_x = 2
    cur_y = 2
    for i in range(GLYPH_COUNT):
        cell = sdf_cell(content_block(glyphs8[FIRST_CODEPOINT + i]))
        for row in range(CELL):
            base = (cur_y + row) * ATLAS_W + cur_x
            coverage[base : base + CELL] = bytes(cell[row * CELL : (row + 1) * CELL])
        records.append(
            {
                "x0": float(cur_x),
                "y0": float(cur_y),
                "x1": float(cur_x + CELL),
                "y1": float(cur_y + CELL),
                "xoff": float(-SDF_PADDING),
                "yoff": float(-SDF_PADDING),
                "xadvance": XADVANCE,
            }
        )
        cur_x += CELL + GUTTER
        if cur_x + CELL + GUTTER > ATLAS_W:
            cur_x = 2
            cur_y += CELL + GUTTER

    if cur_y + CELL > ATLAS_H:
        raise SystemExit("atlas too small for the baked glyph shelf layout")

    header = struct.pack(
        "<6I3f2I",
        FNT0_MAGIC,
        FNT0_VERSION,
        ATLAS_W,
        ATLAS_H,
        GLYPH_COUNT,
        FIRST_CODEPOINT,
        FONT_SIZE,
        BASELINE,
        LINE_HEIGHT,
        FLAG_SDF,
        len(coverage),
    )
    blob = header + b"".join(struct.pack("<7f", *r.values()) for r in records) + bytes(coverage)
    return blob, records


def verify(blob: bytes) -> None:
    magic, version, w, h, count, first, size, baseline, line, flags, px = struct.unpack_from("<6I3f2I", blob, 0)
    assert magic == FNT0_MAGIC and version == FNT0_VERSION
    assert px == w * h and len(blob) == struct.calcsize("<6I3f2I") + count * 28 + px
    glyphs = [struct.unpack_from("<7f", blob, 44 + i * 28) for i in range(count)]

    x0, y0, x1, y1, xoff, yoff, xadv = glyphs[ord("A") - first]
    row_bytes = []
    for y in range(int(y0), int(y1)):
        base = y * w + int(x0)
        row_bytes.append(blob[44 + count * 28 + base : 44 + count * 28 + base + int(x1 - x0)])
    print(f"verified: {w}x{h}, {count} glyphs, first={first}, size={size}, baseline={baseline}, line={line}, flags={flags}")
    print("glyph 'A' distance field (>=128 renders):")
    for row in row_bytes:
        print("".join("#" if v >= 128 else ("+" if v >= 96 else ("." if v >= 32 else " ")) for v in row))


def main() -> int:
    parser = argparse.ArgumentParser(description="Bake the embedded default font (Font8x8 -> cooked FNT0).")
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER, help="Font8x8.hpp to read")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="cooked font to write")
    parser.add_argument("--verify", action="store_true", help="decode the result and print glyph 'A'")
    args = parser.parse_args()

    blob, _ = bake(args.header)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f"wrote {args.out} ({len(blob)} bytes)")
    if args.verify:
        verify(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main())
