#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode the renderer's blue-noise tile to raw 8-bit RGBA at build time.

The renderer needs a blue-noise tile before any asset system exists: it is
uploaded inside RenderContext::Create, and the Kernel constructs its
AssetManager and mounts data/base.pak only after that returns (see
src/engine/Kernel.cpp). So the tile cannot arrive through the VFS, and it ships
inside the binary the way the LTC tables do.

What was wrong was not the embedding, it was the decoding: the PNG was #embeded
verbatim and src/render called stbi_load_from_memory on it at every startup,
which made the renderer an image decoder. This script moves the decode to build
time and emits plain RGBA bytes, so the renderer memcpys a block whose layout it
asked for. For this particular tile that is also strictly cheaper at both ends:
it is 1024x1024 of high-frequency noise, so the PNG (4202841 bytes) is very
nearly the size of the raw pixels (4194304), and the startup decode disappears.

Deliberately stdlib-only. This runs from CMake (see cmake/ShaderCompilation.cmake),
which finds an interpreter but promises no third-party packages, so requiring
Pillow here would make the build depend on something the build does not install.
The decoder below therefore covers exactly what a blue-noise tile is -- 8-bit
truecolour, optionally with alpha, not interlaced -- and fails loudly on
anything else rather than half-decoding it.

Usage:
    cook_blue_noise.py -i <source.png> -o <output.rgba>
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# Colour types this decoder handles: truecolour, and truecolour with alpha.
# Greyscale and palette images would need a different expansion, and a
# blue-noise tile is neither, so they are rejected instead of approximated.
_TRUECOLOUR = 2
_TRUECOLOUR_ALPHA = 6


class CookError(RuntimeError):
    """The input is not a PNG this cook can represent as raw 8-bit RGBA."""


def _chunks(data: bytes):
    """Yield (type, payload) for each chunk, stopping at IEND."""
    offset = 8
    while offset + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, offset)
        ctype = data[offset + 4 : offset + 8]
        payload_start = offset + 8
        payload_end = payload_start + length
        if payload_end + 4 > len(data):
            raise CookError(f"chunk {ctype.decode('latin1')} runs past the end of the file")
        stored_crc = struct.unpack_from(">I", data, payload_end)[0]
        if zlib.crc32(data[offset + 4 : payload_end]) & 0xFFFFFFFF != stored_crc:
            raise CookError(f"chunk {ctype.decode('latin1')} failed its CRC")
        yield ctype, data[payload_start:payload_end]
        if ctype == b"IEND":
            return
        offset = payload_end + 4
    raise CookError("no IEND chunk: truncated PNG")


def _paeth(left: int, above: int, upper_left: int) -> int:
    p = left + above - upper_left
    pa, pb, pc = abs(p - left), abs(p - above), abs(p - upper_left)
    if pa <= pb and pa <= pc:
        return left
    return above if pb <= pc else upper_left


def _unfilter(scanlines: bytes, width: int, height: int, bpp: int, stride: int) -> bytearray:
    """Undo PNG's per-scanline filtering. `stride` excludes the filter byte."""
    out = bytearray(stride * height)
    pos = 0
    previous = bytearray(stride)  # the row above starts as all zeros
    for y in range(height):
        if pos >= len(scanlines):
            raise CookError(f"image data ends at scanline {y} of {height}")
        filter_type = scanlines[pos]
        pos += 1
        raw = scanlines[pos : pos + stride]
        if len(raw) != stride:
            raise CookError(f"scanline {y} is {len(raw)} bytes, expected {stride}")
        pos += stride

        row = bytearray(raw)
        if filter_type == 0:  # None
            pass
        elif filter_type == 1:  # Sub
            for x in range(bpp, stride):
                row[x] = (row[x] + row[x - bpp]) & 0xFF
        elif filter_type == 2:  # Up
            for x in range(stride):
                row[x] = (row[x] + previous[x]) & 0xFF
        elif filter_type == 3:  # Average
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + ((left + previous[x]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                upper_left = previous[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + _paeth(left, previous[x], upper_left)) & 0xFF
        else:
            raise CookError(f"scanline {y} uses unknown filter type {filter_type}")

        out[y * stride : (y + 1) * stride] = row
        previous = row
    return out


def decode_png_rgba(path: Path) -> tuple[int, int, bytes]:
    """Decode `path` to tightly packed 8-bit RGBA. Returns (width, height, pixels)."""
    data = path.read_bytes()
    if not data.startswith(_SIGNATURE):
        raise CookError(f"{path}: not a PNG")

    header = None
    idat = bytearray()
    for ctype, payload in _chunks(data):
        if ctype == b"IHDR":
            if header is not None:
                raise CookError("more than one IHDR")
            if len(payload) != 13:
                raise CookError(f"IHDR is {len(payload)} bytes, expected 13")
            header = struct.unpack(">IIBBBBB", payload)
        elif ctype == b"IDAT":
            idat += payload
        elif ctype == b"PLTE":
            raise CookError("paletted PNG: this cook handles truecolour only")

    if header is None:
        raise CookError("no IHDR chunk")

    width, height, bit_depth, colour_type, compression, filter_method, interlace = header
    if width == 0 or height == 0:
        raise CookError(f"empty image: {width}x{height}")
    if bit_depth != 8:
        raise CookError(f"{bit_depth}-bit samples: this cook handles 8-bit only")
    if colour_type not in (_TRUECOLOUR, _TRUECOLOUR_ALPHA):
        raise CookError(f"colour type {colour_type}: this cook handles truecolour (2) and truecolour+alpha (6)")
    if compression != 0 or filter_method != 0:
        raise CookError(f"unsupported compression {compression} / filter method {filter_method}")
    if interlace != 0:
        raise CookError("interlaced PNG: this cook handles non-interlaced only")

    channels = 4 if colour_type == _TRUECOLOUR_ALPHA else 3
    stride = width * channels
    scanlines = zlib.decompress(bytes(idat))
    pixels = _unfilter(scanlines, width, height, channels, stride)

    if channels == 4:
        return width, height, bytes(pixels)

    # RGB -> RGBA: an opaque alpha for every texel, which is what a
    # truecolour tile with no alpha channel means.
    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        rgba[i * 4 : i * 4 + 3] = pixels[i * 3 : i * 3 + 3]
        rgba[i * 4 + 3] = 0xFF
    return width, height, bytes(rgba)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-i", "--input", required=True, type=Path, help="source PNG")
    parser.add_argument("-o", "--output", required=True, type=Path, help="raw 8-bit RGBA to write")
    args = parser.parse_args(argv)

    try:
        width, height, pixels = decode_png_rgba(args.input)
    except (CookError, OSError, zlib.error) as exc:
        print(f"[cook_blue_noise] ERROR: {exc}", file=sys.stderr)
        return 1

    expected = width * height * 4
    if len(pixels) != expected:
        print(f"[cook_blue_noise] ERROR: decoded {len(pixels)} bytes, expected {expected}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pixels)
    print(f"cook_blue_noise: {args.input.name} -> {args.output} ({width}x{height}, {len(pixels)} bytes RGBA)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
