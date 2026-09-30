#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run with: python3 -m unittest discover -s tests/fidelity -p 'test_olive_layers.py'"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


_TOOL = Path(__file__).resolve().parents[2] / "scripts" / "inspect_olive_layers.py"
_spec = importlib.util.spec_from_file_location("inspect_olive_layers", _TOOL)
assert _spec and _spec.loader
layers = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(layers)


class OliveLayerTest(unittest.TestCase):
    def test_neutral_round_trip_across_branches(self):
        # Both the shared minimum offset and the bright-range peak/desaturation
        # have to be undone jointly, or blue light is attributed to diffuse.
        for hdr in (
            (0.0, 0.0, 0.0),
            (0.0001, 0.0002, 0.001),
            (0.045, 0.075, 0.089),
            (0.08, 0.16, 0.76),
            (0.081, 0.7, 0.8),
            (0.12, 0.25, 0.9),
            (0.07, 0.5, 8.0),
            (3.0, 2.0, 0.4),
            (12.0, 12.0, 12.0),
        ):
            with self.subTest(hdr=hdr):
                recovered = layers.inverse_neutral(layers.neutral_tonemap(hdr))
                for actual, expected in zip(recovered, hdr):
                    self.assertAlmostEqual(actual, expected, delta=max(2e-9, expected * 1e-8))

    def test_srgb_byte_round_trip(self):
        for value in range(256):
            with self.subTest(value=value):
                self.assertEqual(layers.linear_to_srgb_byte(layers.srgb_to_linear(value)), value)

    def test_pam_read_write_and_linear_residual(self):
        width, height = 3, 1
        # Two valid opaque dielectric pixels, one transparent pixel (background).
        diffuse = (0.14, 0.065, 0.035)
        spec = (0.01, 0.03, 0.09)
        total = tuple(a + b for a, b in zip(diffuse, spec))
        opaque = layers.encode_pixel(total) + b"\xff" + layers.encode_pixel(total) + b"\xff" + b"\x00\x00\x00\x00"
        isolated = layers.encode_pixel(spec) + b"\xff" + layers.encode_pixel(spec) + b"\xff" + b"\x00\x00\x00\x00"
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / name for name in ("opaque.pam", "spec.pam", "diffuse.pam")]
            layers.write_pam(paths[0], width, height, opaque)
            layers.write_pam(paths[1], width, height, isolated)
            self.assertEqual(layers.read_pam(paths[0]), (width, height, opaque))
            layers.inspect(paths[0], paths[1], paths[2], [(0, 0), (1, 0)])
            rw, rh, pixels = layers.read_pam(paths[2])
            self.assertEqual((rw, rh), (width, height))
            self.assertEqual(pixels[0:4], pixels[4:8])
            self.assertEqual(pixels[8:12], b"\x00\x00\x00\x00")
            # Display-encoded RGB subtracts incorrectly; inversion before
            # subtraction is required even at moderate intensities.
            self.assertNotEqual(tuple(max(a-b, 0) for a, b in zip(opaque[0:3], isolated[0:3])), tuple(pixels[0:3]))
            for actual, expected in zip(layers.decode_pixel(pixels[0:3]), diffuse):
                self.assertAlmostEqual(actual, expected, delta=0.01)  # 8-bit HDR inverse quantization
            with self.assertRaisesRegex(ValueError, "must not overwrite"):
                layers.inspect(paths[0], paths[1], paths[0], [])

    def test_clipped_pixels_not_used_as_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / name for name in ("opaque.pam", "spec.pam", "diffuse.pam")]
            layers.write_pam(paths[0], 1, 1, bytes((255, 80, 120, 255)))
            layers.write_pam(paths[1], 1, 1, bytes((12, 20, 25, 255)))
            layers.inspect(paths[0], paths[1], paths[2], [(0, 0)])
            self.assertEqual(layers.read_pam(paths[2])[2], bytes((255, 0, 255, 255)))


if __name__ == "__main__":
    unittest.main()
