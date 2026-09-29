#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""CPU checks for filtered GGX IBL sampling and its bake wiring.

Not a shader cook or a GPU/golden-image comparison. The two fidelity scenarios
still need to be captured on a Vulkan device to check the actual reflections.
"""

import math
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (ROOT / path).read_text()


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
    return max(0, min(mip_count - 1, 0.5 * math.log2(sample_solid_angle / texel_solid_angle)))


class IBLFilteredImportanceSamplingTest(unittest.TestCase):
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
        self.assertLess(source_lod(1024, 512, 512, 0.2, 0), 0.2)
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

    def test_bake_uploads_and_exposes_all_source_mips(self) -> None:
        bake = source("src/render/IBLProcessor.hpp")
        self.assertIn("hasRadiance ? GetMipLevels(uploadWidth, uploadHeight) : 1u", bake)
        self.assertIn("VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT", bake)
        self.assertIn("VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT", bake)
        self.assertIn("gpuSourceMips ? ImageUsage::TransferSrc : ImageUsage::None", bake)
        self.assertIn("DownsampleEquirect(mipPixels + sourceMipOffsets[mip - 1]", bake)
        self.assertIn("mipRegion.bufferOffset = sourceMipOffsets[mip] * sizeof(float)", bake)
        self.assertIn("VK_FORMAT_R32G32B32A32_SFLOAT, sourceMipLevels, VK_IMAGE_ASPECT_COLOR_BIT", bake)
        self.assertIn("cmd, radianceImage->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, sourceMipLevels", bake)
        self.assertIn("GenerateMipmaps(cmd, radianceImage->Handle(), uploadWidth, uploadHeight, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)", bake)
        self.assertIn(".mip_count  = sourceMipLevels", bake)  # CPU fallback/1x1 also shader-readable
        heap = source("src/render/init/RenderInitHeaps.cpp")
        sampler = source("src/vulkan/pipeline/SamplerBuilder.cpp")
        self.assertIn("Vk::SamplerBuilder {}.Linear().Info()", heap)
        self.assertIn("VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE", heap)
        self.assertIn(".mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR", sampler)
        self.assertIn(".maxLod                  = VK_LOD_CLAMP_NONE", sampler)

    def test_pdf_lod_and_compute_barrier_match_shader_paths(self) -> None:
        sampling = source("resources/shaders/sampling.slang")
        shader = source("resources/shaders/ibl_bake.slang")
        mipmaps = source("src/vulkan/core/RenderCore.c")
        self.assertIn("float a        = roughness * roughness;", sampling)
        self.assertIn("float alpha  = roughness * roughness;", sampling)
        self.assertIn("return D * NdotH / (4.0f * VdotH);", sampling)
        self.assertIn("radianceMap.GetDimensions(0u, sourceWidth, sourceHeight, sourceMipLevels)", shader)
        self.assertIn("(2.0f * PI * PI) / (float(sourceWidth) * float(sourceHeight))", shader)
        self.assertIn("sin(0.5f * PI / float(sourceHeight))", shader)
        self.assertIn("GGXReflectionPDF(N, V, H, pc.roughness)", shader)
        self.assertIn("1.0f / (float(samples) * max(pdf, 1e-6f))", shader)
        self.assertIn("0.5f * log2(sampleSolidAngle / texelSolidAngle)", shader)
        self.assertIn("float(sourceMipLevels - 1u)", shader)
        self.assertIn("radianceMap.SampleLevel(radianceSampler, float2(u, v), mipLevel)", shader)
        self.assertIn("color = SampleEnvironment(R, 0.0f);", shader)  # polished mip
        self.assertIn("float3 Lin = SampleEnvironment(dir, 0.0f);", shader)  # SH
        self.assertIn("if (pc.hasRadiance != 0u)\n        return SampleEquirect(dir, mipLevel);", shader)
        self.assertEqual(mipmaps.count(".dst_stage  = shaderReadStage"), 2)
        core = source("src/vulkan/core/RenderCore.hpp")
        self.assertIn("shaderReadStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT", core)  # other callers


if __name__ == "__main__":
    unittest.main()
