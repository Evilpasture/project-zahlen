#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard TransmissionRoughnessTest's renderer wiring without a GPU.

This checks source-level contracts and their expected IOR/roughness behavior;
it is not a shader cook or a comparison against Khronos golden images.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (ROOT / path).read_text()


class FidelityTransmissionWiringTest(unittest.TestCase):
    def test_render_target_binds_mip_zero_for_attachment_and_all_mips_for_sampling(self) -> None:
        targets = source("src/render/TargetManager.hpp")
        allocation = source("src/render/TargetManager.cpp")
        views = source("src/vulkan/memory/RenderTarget.hpp")
        resolver = source("src/render/RenderGraphBuilder.cpp")
        heap = source("src/render/init/RenderInitHeaps.cpp")
        sampler = source("src/vulkan/pipeline/SamplerBuilder.cpp")
        uniforms = source("src/render/RenderSetup.cpp")

        self.assertIn("Vk::MipmappedRenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> transLightingTarget", targets)
        self.assertIn('Res_TransLighting = Vk::GraphImage<"TransLighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>', targets)
        self.assertIn("VkExtent2D res    = _impl->graphResources.sceneColor.extent", uniforms)
        self.assertIn("gpuUniforms.screenResolution[0] = static_cast<float>(res.width)", uniforms)
        self.assertIn("gpuUniforms.screenResolution[1] = static_cast<float>(res.height)", uniforms)
        self.assertIn("target.mipLevels = GetMipLevels(extent.width, extent.height)", views)
        self.assertIn("MakeViewCreateInfo2D(target.image.Handle(), F, target.mipLevels, aspect)", views)
        self.assertIn("MakeViewCreateInfo2D(target.image.Handle(), F, 1, aspect, m)", views)
        self.assertIn("Vk::ImageUsage::ColorAttachment | Vk::ImageUsage::Sampled | Vk::ImageUsage::TransferSrc | Vk::ImageUsage::TransferDst", allocation)
        self.assertIn("ResourceResolver<Res_TransLighting>", resolver)
        self.assertIn("target.mipViews[0]", resolver)
        self.assertIn("graphResources.transLightingTarget.fullView", heap)
        self.assertIn("auto clampBuilder = Vk::SamplerBuilder {}.Linear().ClampToEdge();", heap)
        self.assertIn(".mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR", sampler)
        self.assertIn(".maxLod                  = VK_LOD_CLAMP_NONE", sampler)

    def test_copy_builds_mips_and_graph_tracks_shader_read_exit_layout(self) -> None:
        passes = source("src/render/RenderGraphBuilder.cpp")
        copy_decl = source("src/render/passes/forward/OpaqueSceneCopyPass.hpp")
        copy = source("src/render/passes/forward/OpaqueSceneCopyPass.cpp")
        graph = source("src/vulkan/graph/RenderGraph.hpp")
        state = source("src/vulkan/graph/RenderGraph.inl")
        mipmaps = source("src/vulkan/core/RenderCore.c")
        forward = source("src/render/passes/forward/ForwardPass.hpp")

        self.assertLess(passes.index("Passes::OpaqueSceneCopyPass"), passes.index("Passes::ForwardPass"))
        self.assertIn("Vk::TransferDstWriteThenShaderRead<Res_TransLighting>", copy_decl)
        self.assertLess(copy.index("vkCmdCopyImage("), copy.index("Vk::GenerateMipmaps("))
        self.assertIn("if (dst.mipLevels > 1)", copy)
        self.assertIn("Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>", copy)
        self.assertIn("const ZHLN_ImageBarrierDesc barrier_read", mipmaps)
        self.assertIn("const ZHLN_ImageBarrierDesc barrier_last", mipmaps)
        self.assertIn(".base_mip   = mipLevels - 1", mipmaps)
        self.assertIn("VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT", graph)
        self.assertIn(".layout            = U::final_layout", state)
        self.assertIn(".stage             = U::final_stage", state)
        self.assertIn(".access            = U::final_access", state)
        self.assertIn(".mip_count  = VK_REMAINING_MIP_LEVELS", state)
        self.assertIn("Vk::ShaderRead<Res_TransLighting>", forward)

    def test_thin_walled_roughness_and_air_row(self) -> None:
        shade = source("resources/shaders/basic.slang").split("float4 ShadeTransmission(", 1)[1].split("float4 PSForward(", 1)[0]

        self.assertIn("float ior = max(input.volumeParams.y, 1.0f);", shade)
        self.assertIn("float eta = (ior - 1.0f) / (ior + 1.0f);", shade)
        self.assertIn("float3 F0 = float3(eta * eta);", shade)
        self.assertIn("ior > 1.0f ? FresnelSchlickRoughness(NdotV, F0, roughness) : float3(0.0f)", shade)
        self.assertIn("float transRoughness = roughness * saturate(2.0f * (ior - 1.0f));", shade)
        self.assertIn("float maxLod = log2(max(scene.frame.screenResolution.x, scene.frame.screenResolution.y));", shade)
        self.assertIn("float lod = transRoughness * maxLod;", shade)
        self.assertIn("scene.texTransLighting.SampleLevel(scene.clampSampler, refractUV, lod)", shade)
        # The thickness gate affects only the UV offset, not blur or Fresnel.
        self.assertLess(shade.index("if (thickness > 1.0e-5f)"), shade.index("float transRoughness"))

        iors = (1.0, 1.33, 1.5, 2.0)
        f0 = [((ior - 1.0) / (ior + 1.0)) ** 2 for ior in iors]
        self.assertEqual(f0[0], 0.0)
        self.assertAlmostEqual(f0[2], 0.04)  # glass at IOR 1.5
        self.assertEqual(f0, sorted(f0))

        for ior in iors:
            blur = max(0.0, min(1.0, 2.0 * (ior - 1.0)))
            lods = [roughness * blur * 10 for roughness in (0.0, 0.25, 0.5, 1.0)]
            if ior == 1.0:
                self.assertEqual(lods, [0.0] * 4)
            else:
                self.assertEqual(lods, sorted(lods))
                self.assertGreater(lods[-1], lods[1])


if __name__ == "__main__":
    unittest.main()
