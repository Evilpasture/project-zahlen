#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""CPU checks for two-sided glTF normal frames and split-sum IBL wiring.

Sponza's hanging chains and foliage are double-sided MASK materials with normal
and metallic-roughness maps. This tests the shading contracts without bundling
Sponza or claiming to compile Slang / compare rendered golden images.
"""

import math
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (ROOT / path).read_text()


class MaskedIBLWiringTest(unittest.TestCase):
    def test_split_sum_uses_f0_not_a_second_fresnel(self) -> None:
        lut = source("resources/shaders/brdf_lut.slang")
        self.assertIn("A += (1.0f - Fc) * G_Vis;", lut)
        self.assertIn("B += Fc * G_Vis;", lut)

        for shader in ("resources/shaders/reflection.slang", "resources/shaders/basic.slang"):
            text = source(shader)
            self.assertRegex(
                text,
                r"float3 FssEss\s*=\s*F0 \* envBRDF\.x \+ float3\(envBRDF\.y, envBRDF\.y, envBRDF\.y\);",
            )
            self.assertNotRegex(text, r"F_rough \* envBRDF\.x")

        # With bright studio IBL and a mostly metallic, dark chain, using
        # FresnelSchlickRoughness as A's multiplier instead of F0 adds an
        # achromatic reflection that is not in the LUT's decomposition.
        n_dot_v, roughness, a, b = 0.2, 0.35, 0.55, 0.05
        f0 = (0.17, 0.08, 0.04)
        f_rough = tuple(c + (max(1 - roughness, c) - c) * (1 - n_dot_v) ** 5 for c in f0)
        correct = tuple(c * a + b for c in f0)
        doubled = tuple(c * a + b for c in f_rough)
        self.assertGreater(doubled[2], correct[2] * 2)
        self.assertGreater(correct[0] / correct[2], doubled[0] / doubled[2])

    def test_gbuffer_flips_the_whole_normal_mapped_frame_on_backfaces(self) -> None:
        shader = source("resources/shaders/basic.slang")
        gbuffer = shader.split("PSOutput PSMain(", 1)[1].split("float3 RgbMix(", 1)[0]
        self.assertIn("bool isFrontFace: SV_IsFrontFace", gbuffer)
        self.assertIn("RejectMaskedAlpha(material, albedo.a)", gbuffer)
        self.assertLess(gbuffer.index("RejectMaskedAlpha("), gbuffer.index("EvaluateMaterial<ClearCoatMaterial>"))
        self.assertLess(gbuffer.index("float3 B"), gbuffer.index("float facing"))
        self.assertLess(gbuffer.index("float facing"), gbuffer.index("EvaluateMaterial<ClearCoatMaterial>"))
        self.assertIn("float facing = isFrontFace ? 1.0f : -1.0f;", gbuffer)
        for axis, field in (("N", "geometricNormal"), ("T", "tangentDirection"), ("B", "bitangentDirection")):
            self.assertRegex(gbuffer, rf"material\.{field}\s*=\s*{axis} \* facing;")
        self.assertIn("cross(N, T) * (input.tangent.w < 0.0f ? -1.0f : 1.0f)", gbuffer)

        # Nonzero tangent-space X and Y catch the common incorrect approach of
        # reversing only the geometric normal (or N and B but not T).
        map_normal = (0.3, -0.4, math.sqrt(1 - 0.3**2 - 0.4**2))
        frame = ((0.0, 0.0, 1.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0))  # N, T, B

        def transform(sign: int) -> tuple[float, float, float]:
            n, t, b = frame
            return tuple(
                sign * (map_normal[0] * t[i] + map_normal[1] * b[i] + map_normal[2] * n[i])
                for i in range(3)
            )

        front, back = transform(1), transform(-1)
        self.assertEqual(back, tuple(-v for v in front))
        self.assertNotEqual(back, (front[0], front[1], -front[2]))

    def test_forward_uses_same_backface_rule_without_double_flipping_transmission(self) -> None:
        shader = source("resources/shaders/basic.slang")
        forward = shader.split("float4 PSForward(", 1)[1]
        self.assertIn("bool isFrontFace: SV_IsFrontFace", forward)
        self.assertLess(forward.index("return ShadeTransmission(input, N, albedo.rgb, isFrontFace);"), forward.index("N *= facing;"))
        self.assertIn("float facing = isFrontFace ? 1.0f : -1.0f;", forward)
        for axis in "NTB":
            self.assertIn(f"{axis} *= facing;", forward)
        self.assertLess(forward.index("N *= facing;"), forward.index("float NdotV"))


if __name__ == "__main__":
    unittest.main()
