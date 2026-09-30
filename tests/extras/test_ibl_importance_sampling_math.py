#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent numerical checks of GGX PDF normalization and HDR footprint LOD.

This does not inspect source code or claim to test the implementation: zshader
compiles and reflects the Slang, and the Vulkan fidelity captures test the
rendered behavior. These are only analytical cross-checks of the math.
"""

import math
import unittest


def ggx_reflection_pdf(roughness: float, n_dot_h: float, v_dot_h: float) -> float:
    # Half-vector GGX uses alpha = roughness^2. Reflection has the 4(V.H)
    # Jacobian: this PDF is per unit solid angle in L, not in H.
    alpha2 = roughness**4
    d = alpha2 / (math.pi * (1 + n_dot_h**2 * (alpha2 - 1)) ** 2)
    return d * n_dot_h / (4 * v_dot_h)


def equirect_texel_solid_angle(width: int, height: int, direction_y: float) -> float:
    min_sin_theta = math.sin(math.pi / (2 * height))
    sin_theta = max(math.sqrt(max(0, 1 - direction_y**2)), min_sin_theta)
    return 2 * math.pi**2 * sin_theta / (width * height)


def source_lod(width: int, height: int, samples: int, roughness: float, direction_y: float) -> float:
    mip_count = max(width, height).bit_length()
    pdf = ggx_reflection_pdf(roughness, 1, 1)
    sample_solid_angle = 1 / (samples * max(pdf, 1e-6))
    texel_solid_angle = equirect_texel_solid_angle(width, height, direction_y)
    # cmgen overlaps adjacent importance-sample footprints 4x. The mirror
    # pass bypasses this LOD calculation; only rough specular uses it.
    return max(0, min(mip_count - 1, 0.5 * math.log2(4 * sample_solid_angle / texel_solid_angle)))


class IBLImportanceSamplingMathTest(unittest.TestCase):
    def test_pdf_is_normalized_over_reflection_directions(self) -> None:
        # For V=N the distribution is rotationally symmetric. H is halfway
        # between V and L, so N.H = sqrt((1+N.L)/2); integrate over all L,
        # including L below the horizon (those samples are discarded later).
        steps = 200_000
        for roughness in (0.2, 0.5, 1.0):
            def integrand(mu: float) -> float:
                n_dot_h = math.sqrt((1 + mu) / 2)
                if n_dot_h == 0:
                    return 0.0
                return ggx_reflection_pdf(roughness, n_dot_h, n_dot_h)

            delta = 2 / steps
            integral = (integrand(-1) + integrand(1)) / 2
            integral += sum(integrand(-1 + i * delta) for i in range(1, steps))
            self.assertAlmostEqual(2 * math.pi * delta * integral, 1.0, delta=0.0003)

        # At roughness 1 the reflected PDF is uniform on the sphere, D/4.
        self.assertAlmostEqual(ggx_reflection_pdf(1, 0.8, 0.8), 1 / (4 * math.pi))

    def test_lod_preserves_mirror_and_filters_rough_hdr(self) -> None:
        self.assertEqual(source_lod(1, 1, 512, 0.6, 0), 0)
        # A sharp HDR source texel covers nearly the same area as one GGX
        # sample at the first roughness level. With no overlap this stays at
        # source mip 0; fourfold overlap blends roughly one mip instead.
        width, height, samples, roughness = 1024, 512, 512, 0.2
        pdf = ggx_reflection_pdf(roughness, 1, 1)
        unoverlapped = 0.5 * math.log2(
            1 / (samples * pdf * equirect_texel_solid_angle(width, height, 0)))
        self.assertGreater(unoverlapped, 0)
        self.assertLess(unoverlapped, 0.1)
        self.assertAlmostEqual(source_lod(width, height, samples, roughness, 0), unoverlapped + 1)
        self.assertGreater(source_lod(1024, 512, 1024, 0.8, 0), 2)
        self.assertLess(source_lod(1024, 512, 1024, 0.8, 0), 10)
        # Finer source texels at high latitude have smaller solid angles.
        self.assertGreater(source_lod(1024, 512, 1024, 0.8, 1), source_lod(1024, 512, 1024, 0.8, 0))
        self.assertLess(source_lod(1024, 512, 2048, 0.8, 0), source_lod(1024, 512, 1024, 0.8, 0))

        # Summing actual latitude-row areas recovers the sphere's 4*pi sr.
        width, height = 1024, 512
        area = sum(width * equirect_texel_solid_angle(width, height, math.cos(math.pi * (y + 0.5) / height))
                   for y in range(height))
        self.assertAlmostEqual(area, 4 * math.pi, delta=0.0001)

if __name__ == "__main__":
    unittest.main()
