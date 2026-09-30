#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reconstruct the opaque olives' non-specular light from fidelity captures.

The existing --diagnostic=dielectric-specular hides glass and zeros the opaque
material's base color. For the olives (metallic texture B == 0), F0 is still
0.04, so this leaves their dielectric specular contribution unchanged. Subtract
it from --diagnostic=opaque-only *before* tone mapping to see what is left.

Both captures must use the same camera, exposure, environment and AA setting;
prefer --no-aa to avoid spatial mixing at edges. This is an analysis tool, not
a renderer change or a valid diffuse-only view of metallic materials. Run
--help for the CLI.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path


def read_pam(path: Path) -> tuple[int, int, bytes]:
    with path.open("rb") as file:
        if file.readline() != b"P7\n":
            raise ValueError(f"{path}: expected a binary P7 PAM")
        header: dict[str, str] = {}
        while True:
            line = file.readline()
            if not line:
                raise ValueError(f"{path}: missing ENDHDR")
            line = line.strip()
            if line == b"ENDHDR":
                break
            if not line or line.startswith(b"#"):
                continue
            key, _, value = line.partition(b" ")
            if not value or key.decode("ascii") in header:
                raise ValueError(f"{path}: invalid PAM header")
            header[key.decode("ascii")] = value.decode("ascii")
        try:
            width = int(header["WIDTH"])
            height = int(header["HEIGHT"])
            depth = int(header["DEPTH"])
            maxval = int(header["MAXVAL"])
        except (KeyError, ValueError) as error:
            raise ValueError(f"{path}: invalid PAM dimensions/format") from error
        if (width < 1 or height < 1 or width * height > 100_000_000 or depth != 4 or maxval != 255 or
                header.get("TUPLTYPE") != "RGB_ALPHA"):
            raise ValueError(f"{path}: expected 8-bit RGB_ALPHA PAM")
        pixels = file.read()
        if len(pixels) != width * height * 4:
            raise ValueError(f"{path}: wrong raster length")
    return width, height, pixels


def write_pam(path: Path, width: int, height: int, pixels: bytes) -> None:
    with path.open("wb") as file:
        file.write(f"P7\nWIDTH {width}\nHEIGHT {height}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n".encode("ascii"))
        file.write(pixels)


def srgb_to_linear(value: int) -> float:
    encoded = value / 255.0
    return encoded / 12.92 if encoded <= 0.04045 else ((encoded + 0.055) / 1.055) ** 2.4


def linear_to_srgb_byte(value: float) -> int:
    value = max(0.0, value)
    encoded = 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055
    return round(min(encoded, 1.0) * 255.0)


def neutral_tonemap(hdr: tuple[float, float, float]) -> tuple[float, float, float]:
    """Mirror NeutralTonemap in resources/shaders/blit.slang for tests/output."""
    low = min(hdr)
    offset = low - 6.25 * low * low if low < 0.08 else 0.04
    color = tuple(component - offset for component in hdr)
    peak = max(color)
    if peak >= 0.76:
        new_peak = 1.0 - (0.24 * 0.24) / (peak - 0.52)
        desaturation = 1.0 - 1.0 / (0.15 * (peak - new_peak) + 1.0)
        color = tuple((component * new_peak / peak) * (1.0 - desaturation) +
                      new_peak * desaturation for component in color)
    return color


def inverse_neutral(rgb: tuple[float, float, float]) -> tuple[float, float, float]:
    """Undo the neutral curve *jointly*, including peak compression/desaturation.

    Inverting each channel separately loses the shared minimum-channel offset
    and overstates the residual blue. Values clipped to 255 are not invertible.
    """
    peak = max(rgb)
    if peak >= 0.76:
        # newPeak = 1 - 0.24^2 / (peak + 0.24 - 0.76).
        original_peak = 0.52 + (0.24 * 0.24) / (1.0 - peak)
        desaturation = 1.0 - 1.0 / (0.15 * (original_peak - peak) + 1.0)
        scale = (1.0 - desaturation) * peak / original_peak
        shifted = tuple((component - desaturation * peak) / scale for component in rgb)
    else:
        shifted = rgb

    minimum = min(shifted)
    # For x < 0.08, min(x - offset(x)) = 6.25*x^2; else it is x - 0.04.
    if minimum < 0.04:
        original_minimum = math.sqrt(max(minimum, 0.0) / 6.25)
        offset = original_minimum - minimum
    else:
        offset = 0.04
    return tuple(component + offset for component in shifted)


_LINEAR_BYTES = tuple(srgb_to_linear(byte) for byte in range(256))


def decode_pixel(pixel: bytes) -> tuple[float, float, float]:
    return inverse_neutral((_LINEAR_BYTES[pixel[0]], _LINEAR_BYTES[pixel[1]], _LINEAR_BYTES[pixel[2]]))


def encode_pixel(hdr: tuple[float, float, float]) -> bytes:
    return bytes(linear_to_srgb_byte(channel) for channel in neutral_tonemap(hdr))


def residual(opaque: tuple[float, float, float], specular: tuple[float, float, float]) -> tuple[float, float, float]:
    return tuple(max(0.0, total - spec) for total, spec in zip(opaque, specular))


def parse_point(text: str) -> tuple[int, int]:
    try:
        x, y = (int(part) for part in text.split(","))
        if x < 0 or y < 0:
            raise ValueError
        return x, y
    except ValueError as error:
        raise argparse.ArgumentTypeError("point must be X,Y in original image pixels") from error


def inspect(opaque_path: Path, specular_path: Path, output_path: Path, points: list[tuple[int, int]]) -> None:
    width, height, opaque = read_pam(opaque_path)
    spec_width, spec_height, specular = read_pam(specular_path)
    if (width, height) != (spec_width, spec_height):
        raise ValueError("captures must have identical dimensions")
    if output_path.resolve() in (opaque_path.resolve(), specular_path.resolve()):
        raise ValueError("the output must not overwrite either input")
    if any(x >= width or y >= height for x, y in points):
        raise ValueError(f"sample point outside {width}x{height} image")

    out = bytearray(len(opaque))
    inspected: dict[int, tuple[tuple[float, ...], tuple[float, ...], tuple[float, ...]]] = {}
    requested = {y * width + x for x, y in points}
    skipped_alpha = skipped_clipped = 0
    for n in range(width * height):
        offset = n * 4
        a = opaque[offset:offset + 4]
        b = specular[offset:offset + 4]
        if a[3] != 255 or b[3] != 255:
            skipped_alpha += 1
            out[offset:offset + 4] = bytes((0, 0, 0, 0))
            continue
        # >=255 is irrecoverably clipped; <=254 can still be very sensitive
        # near peak=1, so callers should inspect moderate-brightness pixels.
        if 255 in a[:3] or 255 in b[:3]:
            skipped_clipped += 1
            out[offset:offset + 4] = bytes((255, 0, 255, 255))
            continue
        total = decode_pixel(a)
        spec = decode_pixel(b)
        diffuse = residual(total, spec)
        out[offset:offset + 3] = encode_pixel(diffuse)
        out[offset + 3] = 255
        if n in requested:
            inspected[n] = total, spec, diffuse

    write_pam(output_path, width, height, out)
    print(f"Wrote {output_path} ({width}x{height}); {skipped_alpha} transparent/alpha-mismatched and "
          f"{skipped_clipped} clipped pixels marked transparent/magenta.")
    for x, y in points:
        n = y * width + x
        if n not in inspected:
            print(f"({x}, {y}): unavailable (transparent, alpha mismatch, or clipped)")
            continue
        total, spec, diffuse = inspected[n]
        pix = n * 4
        print(f"({x}, {y}) display RGB: opaque={tuple(opaque[pix:pix+3])}, "
              f"dielectric-specular={tuple(specular[pix:pix+3])}, residual={tuple(out[pix:pix+3])}")
        print(f"         HDR linear: opaque={tuple(round(v, 4) for v in total)}, "
              f"specular={tuple(round(v, 4) for v in spec)}, "
              f"residual={tuple(round(v, 4) for v in diffuse)}")
        print(f"         specular/opaque RGB: {tuple(round(100 * spec[i] / total[i], 1) if total[i] > 1e-8 else 0.0 for i in range(3))}%")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("opaque_only", type=Path, help="olives-opaque-only.pam (--no-aa)")
    parser.add_argument("dielectric_specular", type=Path, help="olives-dielectric-specular.pam (same settings)")
    parser.add_argument("--output", type=Path, required=True, help="write reconstructed residual as a new .pam")
    parser.add_argument("--point", type=parse_point, action="append", default=[], metavar="X,Y",
                        help="report contributions at an interior olive pixel; repeat to sample several")
    args = parser.parse_args()
    try:
        inspect(args.opaque_only, args.dielectric_specular, args.output, args.point)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
