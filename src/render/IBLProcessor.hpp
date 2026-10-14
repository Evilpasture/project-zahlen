// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <expected>
#include <numbers>
#include <span>
#include <utility>

namespace ZHLN {

inline constexpr uint32_t kMaxEnvironmentRadianceExtent = 8192;

enum class EnvironmentBakeError : uint8_t {
    RadianceTooLarge             ZHLN_ANNOTATION(ZHLN::Description<"radiance equirect exceeds the bake size limit"> {}) = 1,
    InvalidRadianceData          ZHLN_ANNOTATION(ZHLN::Description<"radiance float4 data does not match its extent"> {}),
    RadianceFilteringUnsupported ZHLN_ANNOTATION(ZHLN::Description<"radiance float format does not support linear sampling"> {}),
};

class IBLProcessor {
  public:
    struct RadianceSource {
        const float*                          rgba;
        const float*                          visualRgba;
        std::span<const std::array<float, 3>> diffuseSH;
        uint32_t                              width;
        uint32_t                              height;
        int                                   renderSkybox;
    };

    static auto Bake(RenderContext::Impl& impl, const EnvironmentSettings& sky = {}, const RadianceSource& radiance = {})
        -> std::expected<IBLPayload, ZHLN::ErrorCode> {
        constexpr uint32_t kLutSize   = 512;
        constexpr uint32_t kBaseSize  = 256;
        constexpr uint32_t kMipLevels = 6;
        constexpr size_t   kSHBytes   = sizeof(JPH::Vec4) * 9;

        const bool hasRadiance   = radiance.rgba != nullptr && radiance.width > 0 && radiance.height > 0;
        const bool hasPreparedSH = radiance.diffuseSH.size() == 9;
        if (hasRadiance && (radiance.width > kMaxEnvironmentRadianceExtent || radiance.height > kMaxEnvironmentRadianceExtent)) {
            return std::unexpected(EnvironmentBakeError::RadianceTooLarge);
        }

        const VkFormat cubeFormat      = hasRadiance ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const bool     useVisualSky    = hasRadiance && radiance.visualRgba != nullptr && radiance.renderSkybox != 0;
        const int      environmentMode = hasRadiance ? (radiance.renderSkybox == 0 ? 2 : (useVisualSky ? 3 : 1)) : 0;

        if (hasRadiance) {
            ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips from a {}x{} radiance map...", radiance.width, radiance.height);
        } else {
            ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips (procedural sky)...");
        }

        // 1. Compile Bake Compute Pipelines
        auto makePass = [&](const Vk::ShaderDesc& shader, const auto& bindings) {
            return CreateHeapComputePass(impl.ctx.Device(), shader, bindings.GetInfo(), bindings.indexPushOffset, &impl.pipelineBinaries);
        };
        auto brdfPass = makePass(Vk::CreateShaderDesc<Shaders::Modules::BrdfLutCS>(), impl.bakeHeapBindings);
        auto specPass = makePass(Vk::CreateShaderDesc<Shaders::Modules::IblSpecularCS>(), impl.iblBakeHeapBindings);
        auto shPass   = makePass(Vk::CreateShaderDesc<Shaders::Modules::IblShCS>(), impl.iblBakeHeapBindings);
        if (!brdfPass || !specPass || !shPass) {
            return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
        }

        // 2. Allocate Output Targets & Scratch Storage
        IBLPayload payload;
        Vk::Buffer shBuffer;

        ZHLN::defer rollback([&] {
            payload.Destroy(impl.allocator);
            impl.allocator.DestroyBuffer(shBuffer);
        });

        auto lutImg = Vk::Image::Create(
            impl.allocator, Vk::ImageConfig::Texture2D({kLutSize, kLutSize}, VK_FORMAT_R8G8B8A8_UNORM, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled)
        );
        auto specImg =
            Vk::Image::Create(impl.allocator, Vk::ImageConfig::Cube(kBaseSize, cubeFormat, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled, kMipLevels));
        if (!lutImg || !specImg) {
            return std::unexpected(lutImg ? specImg.error() : lutImg.error());
        }

        payload.brdfLutImage      = std::move(*lutImg);
        payload.prefilteredImage  = std::move(*specImg);
        payload.prefilteredFormat = cubeFormat;
        payload.environmentMode   = environmentMode;

        if (useVisualSky) {
            auto skyImg = Vk::Image::Create(
                impl.allocator, Vk::ImageConfig::Texture2D(
                                    {radiance.width, radiance.height}, VK_FORMAT_R32G32B32A32_SFLOAT, Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled
                                )
            );
            if (!skyImg) {
                return std::unexpected(skyImg.error());
            }
            payload.visualSkyImage = std::move(*skyImg);
        }

        if (!hasPreparedSH) {
            // Directly write to host-visible memory over BDA: eliminates the GPU-to-CPU buffer copy and extra barrier
            auto shBufRes =
                Vk::Buffer::Create(impl.allocator, kSHBytes, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUToCPU);
            if (!shBufRes) {
                return std::unexpected(shBufRes.error());
            }
            shBuffer = std::move(*shBufRes);
        }

        // 3. Stage & Upload Radiance Input
        auto stagedInput = StageRadianceInput(impl, radiance, hasRadiance, useVisualSky);
        if (!stagedInput) {
            return std::unexpected(stagedInput.error());
        }
        ZHLN::defer cleanupStaging([&] { stagedInput->Destroy(impl.allocator); });

        // 4. Descriptor Heap Setups
        const auto        toVec4 = [](std::span<const float, 4> v) { return JPH::Vec4(v[0], v[1], v[2], v[3]); };
        const IBLBakePush basePush {
            .outAddr     = hasPreparedSH ? 0 : impl.ctx.BufferAddress(shBuffer.Handle()),
            .sampleCount = 16384,
            .hasRadiance = hasRadiance ? 1u : 0u,
            .skyZenith   = toVec4(sky.skyZenith),
            .skyHorizon  = toVec4(sky.skyHorizon),
            .skyGround   = toVec4(sky.skyGround),
            .sunDir      = JPH::Vec4(JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized(), 0.0f),
        };

        impl.heapManager.BeginImmediate();
        const auto brdfBlock =
            impl.heapManager.WriteHeapParameters<Shaders::Bake>(impl.bakeHeapBindings, Vk::Slot<"outTexture">(payload.brdfLutImage.ViewInfo()));

        const auto                                radianceView = stagedInput->image.ViewInfo(VK_IMAGE_ASPECT_COLOR_BIT, 0, stagedInput->mipLevels);
        std::array<Vk::HeapBlockBase, kMipLevels> specMipBlocks {};
        for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
            const auto mipView = payload.prefilteredImage.ViewInfo(VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
            specMipBlocks[mip] = impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
                impl.iblBakeHeapBindings, Vk::Slot<"outTexture">(mipView), Vk::Slot<"radianceMap">(radianceView)
            );
        }

        const auto shBlock =
            !hasPreparedSH ? impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
                                 impl.iblBakeHeapBindings,
                                 Vk::Slot<"outTexture">(payload.prefilteredImage.ViewInfo(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6, VK_IMAGE_VIEW_TYPE_2D_ARRAY)),
                                 Vk::Slot<"radianceMap">(radianceView)
                             ) :
                             Vk::HeapBlockBase {};

        // 5. Execute Bake Dispatches
        ExecuteImmediate(impl.ctx, impl.graphicsCmdRing, [&](VkCommandBuffer cmd) {
            impl.heapManager.BindHeaps(cmd);

            stagedInput->RecordUpload(cmd, useVisualSky ? payload.visualSkyImage.Handle() : VK_NULL_HANDLE);

            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, payload.brdfLutImage.Handle());
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, payload.prefilteredImage.Handle());

            brdfPass->DispatchHeapIndexedThreads<Shaders::Modules::BrdfLutCS>(
                cmd, brdfBlock, kLutSize, kLutSize, 1, BRDFLUTPush {.width = kLutSize, .height = kLutSize, .sampleCount = 128}
            );

            if (!hasPreparedSH) {
                shPass->DispatchHeapIndexedThreads<Shaders::Modules::IblShCS>(cmd, shBlock, 64, 1, 1, basePush);
            }

            for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                const uint32_t mipSize     = kBaseSize >> mip;
                const uint32_t sampleCount = (mip == 0) ? 1u : (hasRadiance ? (mip >= 3 ? 1024u : 512u) : 32u);

                IBLBakePush push = basePush;
                push.width       = mipSize;
                push.height      = mipSize;
                push.roughness   = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);
                push.sampleCount = sampleCount;

                for (uint32_t face = 0; face < 6; ++face) {
                    push.face = face;
                    specPass->DispatchHeapIndexedThreads<Shaders::Modules::IblSpecularCS>(cmd, specMipBlocks[mip], mipSize, mipSize, 1, push);
                }
            }

            Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, payload.brdfLutImage.Handle());
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, payload.prefilteredImage.Handle());
        });

        // 6. Finalize & Read Back SH
        if (!hasPreparedSH) {
            auto mappedSH = shBuffer.Map(impl.allocator);
            if (!mappedSH) {
                return std::unexpected(mappedSH.error());
            }
            std::memcpy(payload.shCoeffs.data(), mappedSH->Data(), kSHBytes);
        } else {
            for (size_t c = 0; c < payload.shCoeffs.size(); ++c) {
                const auto& rgb     = radiance.diffuseSH[c];
                payload.shCoeffs[c] = JPH::Vec4(rgb[0], rgb[1], rgb[2], 0.0f);
            }
        }

        payload.brdfLutInfo     = payload.brdfLutImage.ViewInfo();
        payload.prefilteredInfo = payload.prefilteredImage.ViewInfo(VK_IMAGE_ASPECT_COLOR_BIT, 0, kMipLevels);
        if (useVisualSky) {
            payload.visualSkyInfo = payload.visualSkyImage.ViewInfo();
        }

        rollback.Dismiss();
        impl.allocator.DestroyBuffer(shBuffer);
        return payload;
    }

  private:
    struct StagedRadiance {
        Vk::Image                                                                                          image;
        Vk::Buffer                                                                                         staging;
        uint32_t                                                                                           width         = 1;
        uint32_t                                                                                           height        = 1;
        uint32_t                                                                                           mipLevels     = 1;
        bool                                                                                               gpuSourceMips = false;
        bool                                                                                               cpuSourceMips = false;
        size_t                                                                                             visualOffset  = 0;
        std::array<size_t, Vk::GetMipLevels(kMaxEnvironmentRadianceExtent, kMaxEnvironmentRadianceExtent)> mipOffsets {};

        void Destroy(Vk::Allocator& allocator) noexcept {
            allocator.DestroyImage(image);
            allocator.DestroyBuffer(staging);
        }

        void RecordUpload(VkCommandBuffer cmd, VkImage visualSky) const noexcept {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels);

            const VkBufferImageCopy2 region {
                .sType            = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                .imageExtent      = {width, height, 1},
            };
            Vk::CopyBufferToImage(cmd, staging.Handle(), image.Handle(), region);

            if (gpuSourceMips) {
                Vk::GenerateMipmaps(cmd, image.Handle(), width, height, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            } else {
                if (cpuSourceMips) {
                    for (uint32_t mip = 1; mip < mipLevels; ++mip) {
                        VkBufferImageCopy2 mipRegion        = region;
                        mipRegion.bufferOffset              = mipOffsets[mip] * sizeof(float);
                        mipRegion.imageSubresource.mipLevel = mip;
                        mipRegion.imageExtent               = {.width = std::max(1u, width >> mip), .height = std::max(1u, height >> mip), .depth = 1u};
                        Vk::CopyBufferToImage(cmd, staging.Handle(), image.Handle(), mipRegion);
                    }
                }
                Vk::ImageBarrier(
                    cmd, Vk::ImageBarrierDesc {
                             .image      = image.Handle(),
                             .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                             .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                             .base_mip   = 0,
                             .mip_count  = mipLevels
                         }
                );
            }

            if (visualSky != VK_NULL_HANDLE) {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, visualSky);
                auto visualRegion         = region;
                visualRegion.bufferOffset = visualOffset * sizeof(float);
                Vk::CopyBufferToImage(cmd, staging.Handle(), visualSky, visualRegion);
                Vk::ImageBarrier(
                    cmd, {.image      = visualSky,
                          .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                          .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          .dst_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                          .base_mip   = 0,
                          .mip_count  = 1}
                );
            }
        }
    };

    static auto StageRadianceInput(RenderContext::Impl& impl, const RadianceSource& src, bool hasRadiance, bool useVisualSky)
        -> std::expected<StagedRadiance, ErrorCode> {
        StagedRadiance out {
            .width     = hasRadiance ? src.width : 1u,
            .height    = hasRadiance ? src.height : 1u,
            .mipLevels = hasRadiance ? Vk::GetMipLevels(src.width, src.height) : 1u,
        };

        if (hasRadiance) {
            VkFormatProperties props {};
            vkGetPhysicalDeviceFormatProperties(impl.ctx.Physical(), VK_FORMAT_R32G32B32A32_SFLOAT, &props);
            if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
                return std::unexpected(EnvironmentBakeError::RadianceFilteringUnsupported);
            }
            constexpr auto kBlitFlags = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
            out.gpuSourceMips         = (out.mipLevels > 1) && ((props.optimalTilingFeatures & kBlitFlags) == kBlitFlags);
            out.cpuSourceMips         = (out.mipLevels > 1) && !out.gpuSourceMips;
        }

        size_t stagedFloats = static_cast<size_t>(out.width) * out.height * 4u;
        if (out.cpuSourceMips) {
            for (uint32_t mip = 1; mip < out.mipLevels; ++mip) {
                out.mipOffsets[mip] = stagedFloats;
                stagedFloats += static_cast<size_t>(std::max(1u, out.width >> mip)) * std::max(1u, out.height >> mip) * 4u;
            }
        }
        out.visualOffset = stagedFloats;
        if (useVisualSky) {
            stagedFloats += static_cast<size_t>(out.width) * out.height * 4u;
        }

        auto imgRes = Vk::Image::Create(
            impl.allocator,
            Vk::ImageConfig::Texture2D(
                {out.width, out.height}, VK_FORMAT_R32G32B32A32_SFLOAT,
                Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled | (out.gpuSourceMips ? Vk::ImageUsage::TransferSrc : Vk::ImageUsage::None), out.mipLevels
            )
        );
        auto stgRes = Vk::Buffer::Create(impl.allocator, stagedFloats * sizeof(float), Vk::BufferUsage::TransferSrc, Vk::MemoryUsage::CPUOnly);
        if (!imgRes || !stgRes) {
            if (imgRes) {
                impl.allocator.DestroyImage(*imgRes);
            }
            return std::unexpected(imgRes ? stgRes.error() : imgRes.error());
        }

        out.image   = std::move(*imgRes);
        out.staging = std::move(*stgRes);

        auto mapped = out.staging.Map(impl.allocator);
        if (!mapped) {
            out.Destroy(impl.allocator);
            return std::unexpected(mapped.error());
        }

        std::array<float, 4> dummy {};
        const float*         pixels = hasRadiance ? src.rgba : dummy.data();
        std::memcpy(mapped->Data(), pixels, static_cast<size_t>(out.width) * out.height * sizeof(float) * 4u);

        if (out.cpuSourceMips) {
            auto* mipPixels = mapped->As<float>();
            for (uint32_t mip = 1; mip < out.mipLevels; ++mip) {
                DownsampleEquirect(
                    mipPixels + out.mipOffsets[mip - 1], mipPixels + out.mipOffsets[mip], std::max(1u, out.width >> (mip - 1)),
                    std::max(1u, out.height >> (mip - 1)), std::max(1u, out.width >> mip), std::max(1u, out.height >> mip)
                );
            }
        }
        if (useVisualSky) {
            std::memcpy(mapped->As<float>() + out.visualOffset, src.visualRgba, static_cast<size_t>(out.width) * out.height * sizeof(float) * 4u);
        }
        return out;
    }

    static void DownsampleEquirect(const float* src, float* dst, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) noexcept {
        constexpr float pi = std::numbers::pi_v<float>;

        // Cast dimensions once upfront
        const auto srcW_f = static_cast<float>(srcW);
        const auto srcH_f = static_cast<float>(srcH);
        const auto dstW_f = static_cast<float>(dstW);
        const auto dstH_f = static_cast<float>(dstH);

        for (uint32_t y = 0; y < dstH; ++y) {
            const auto   y_f          = static_cast<float>(y);
            const float  top          = y_f * srcH_f / dstH_f;
            const float  bottom       = (y_f + 1.0f) * srcH_f / dstH_f;
            const size_t dstRowOffset = static_cast<size_t>(y) * dstW * 4u;

            for (uint32_t x = 0; x < dstW; ++x) {
                const auto  x_f   = static_cast<float>(x);
                const float left  = x_f * srcW_f / dstW_f;
                const float right = (x_f + 1.0f) * srcW_f / dstW_f;

                std::array<double, 4> sum {};
                double                weightSum = 0.0;

                const auto syStart = static_cast<uint32_t>(top);
                for (uint32_t sy = syStart; sy < srcH; ++sy) {
                    const auto sy_f = static_cast<float>(sy);
                    if (sy_f >= bottom) {
                        break;
                    }

                    const float  rowOverlap   = std::min(sy_f + 1.0f, bottom) - std::max(sy_f, top);
                    const float  rowWeight    = rowOverlap * std::sin(pi * (sy_f + 0.5f) / srcH_f);
                    const size_t srcRowOffset = static_cast<size_t>(sy) * srcW;

                    const auto sxStart = static_cast<uint32_t>(left);
                    for (uint32_t sx = sxStart; sx < srcW; ++sx) {
                        const auto sx_f = static_cast<float>(sx);
                        if (sx_f >= right) {
                            break;
                        }

                        const float  colOverlap = std::min(sx_f + 1.0f, right) - std::max(sx_f, left);
                        const auto   weight     = static_cast<double>(rowWeight * colOverlap);
                        const size_t index      = (srcRowOffset + sx) * 4u;

                        sum[0] += static_cast<double>(src[index + 0]) * weight;
                        sum[1] += static_cast<double>(src[index + 1]) * weight;
                        sum[2] += static_cast<double>(src[index + 2]) * weight;
                        sum[3] += static_cast<double>(src[index + 3]) * weight;
                        weightSum += weight;
                    }
                }

                const size_t dstIndex  = dstRowOffset + (static_cast<size_t>(x) * 4u);
                const double invWeight = weightSum > 0.0 ? (1.0 / weightSum) : 0.0;

                dst[dstIndex + 0] = static_cast<float>(sum[0] * invWeight);
                dst[dstIndex + 1] = static_cast<float>(sum[1] * invWeight);
                dst[dstIndex + 2] = static_cast<float>(sum[2] * invWeight);
                dst[dstIndex + 3] = static_cast<float>(sum[3] * invWeight);
            }
        }
    }

    struct BRDFLUTPush {
        uint32_t width       = 0;
        uint32_t height      = 0;
        uint32_t sampleCount = 0;
    };
    static_assert(GpuAbi::ScenePassPayload<BRDFLUTPush>);

    struct IBLBakePush {
        uint64_t  outAddr     = 0;
        uint32_t  width       = 0;
        uint32_t  height      = 0;
        float     roughness   = 0.0f;
        uint32_t  face        = 0;
        uint32_t  sampleCount = 0;
        uint32_t  hasRadiance = 0;
        JPH::Vec4 skyZenith   = JPH::Vec4::sZero();
        JPH::Vec4 skyHorizon  = JPH::Vec4::sZero();
        JPH::Vec4 skyGround   = JPH::Vec4::sZero();
        JPH::Vec4 sunDir      = JPH::Vec4::sZero();
    };
    static_assert(GpuAbi::ScenePassPayload<IBLBakePush>);
    static_assert(sizeof(IBLBakePush) == 96);
};

} // namespace ZHLN
