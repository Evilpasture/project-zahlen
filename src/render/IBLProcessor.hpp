// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include "Resources.hpp"
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
#include <span>
#include <utility>

namespace ZHLN::Vk {

// GPU bake budget for caller-supplied linear environment pixels.
inline constexpr uint32_t kMaxEnvironmentRadianceExtent = 8192;

enum class EnvironmentBakeError : uint8_t {
    RadianceTooLarge ZHLN_ANNOTATION(ZHLN::Description<"radiance equirect exceeds the bake size limit"> {}) = 1,
    InvalidRadianceData ZHLN_ANNOTATION(ZHLN::Description<"radiance float4 data does not match its extent"> {}),
    RadianceFilteringUnsupported ZHLN_ANNOTATION(ZHLN::Description<"radiance float format does not support linear sampling"> {}),
};

class IBLProcessor {
  public:
    struct RadianceSource {
        // Conditioned pixels drive both GPU bake paths. visualRgba is the
        // original only when it differs from the lighting panorama.
        const float* rgba;
        const float* visualRgba;
        std::span<const std::array<float, 3>> diffuseSH;
        uint32_t     width;
        uint32_t     height;
        int          renderSkybox;
    };

    static auto Bake(RenderContext::Impl& impl, const EnvironmentSettings& sky = {}, const RadianceSource& radiance = {})
        -> std::expected<IBLPayload, ZHLN::ErrorCode> {
        constexpr uint32_t kLutSize   = 512;
        constexpr uint32_t kBaseSize  = 256;
        constexpr uint32_t kMipLevels = 6;
        constexpr size_t   kSHBytes   = sizeof(JPH::Vec4) * 9;

        const bool hasRadiance = radiance.rgba != nullptr && radiance.width > 0 && radiance.height > 0;
        const bool hasPreparedSH = radiance.diffuseSH.size() == 9;
        if (hasRadiance && (radiance.width > kMaxEnvironmentRadianceExtent || radiance.height > kMaxEnvironmentRadianceExtent)) {
            return std::unexpected(EnvironmentBakeError::RadianceTooLarge);
        }
        const VkFormat cubeFormat = hasRadiance ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const bool useVisualSky = hasRadiance && radiance.visualRgba != nullptr && radiance.renderSkybox != 0;
        const int environmentMode = hasRadiance ? (radiance.renderSkybox == 0 ? 2 : (useVisualSky ? 3 : 1)) : 0;
        const uint32_t hasRadianceWord = hasRadiance ? 1u : 0u;

        if (hasRadiance) {
            ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips from a {}x{} radiance map...", radiance.width, radiance.height);
        } else {
            ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips (procedural sky)...");
        }

        const auto requireShader = [](const ShaderDesc& shader) -> std::expected<ShaderDesc, ZHLN::ErrorCode> {
            if (shader.code == nullptr || shader.size == 0) {
                return std::unexpected(ZHLN::Vk::ShaderStageCreationError::ShaderLoadingFailed);
            }
            return shader;
        };

        const auto      brdfShader = Vk::CreateShaderDesc<Shaders::Modules::BrdfLutCS>();
        const auto      specShader = Vk::CreateShaderDesc<Shaders::Modules::IblSpecularCS>();
        const auto      shShader   = Vk::CreateShaderDesc<Shaders::Modules::IblShCS>();
        const JPH::Vec4 sunDir     = JPH::Vec4(JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized(), 0.0f);
        const auto toVec4 = [](const std::array<float, 4>& values) {
            return JPH::Vec4(values[0], values[1], values[2], values[3]);
        };
        const JPH::Vec4 skyZenith  = toVec4(sky.skyZenith);
        const JPH::Vec4 skyHorizon = toVec4(sky.skyHorizon);
        const JPH::Vec4 skyGround  = toVec4(sky.skyGround);

        // These pipeline wrappers are RAII Vulkan objects. The VMA resources
        // below are not; their lexical guard covers every early return.
        for (const auto shader: {brdfShader, specShader, shShader}) {
            if (auto checked = requireShader(shader); !checked) {
                return std::unexpected(checked.error());
            }
        }
        auto brdfPass = CreateHeapComputePass(
            impl.ctx.Device(), brdfShader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
        );
        if (!brdfPass) return std::unexpected(brdfPass.error());
        auto specPass = CreateHeapComputePass(
            impl.ctx.Device(), specShader, impl.iblBakeHeapBindings.GetInfo(), impl.iblBakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
        );
        if (!specPass) return std::unexpected(specPass.error());
        auto shPass = CreateHeapComputePass(
            impl.ctx.Device(), shShader, impl.iblBakeHeapBindings.GetInfo(), impl.iblBakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
        );
        if (!shPass) return std::unexpected(shPass.error());

        struct State {
            Buffer     shGpu;
            Buffer     shCpu;
            IBLPayload payload;
        } state;
        ZHLN::defer _([&] {
            state.payload.Destroy(impl.allocator);
            impl.allocator.DestroyBuffer(state.shGpu);
            impl.allocator.DestroyBuffer(state.shCpu);
        });

        if (!hasPreparedSH) {
            auto shGpu = Buffer::Create(
                impl.allocator, kSHBytes,
                BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst | BufferUsage::ShaderDeviceAddress,
                MemoryUsage::GPUOnly
            );
            if (!shGpu) return std::unexpected(shGpu.error());
            state.shGpu = std::move(*shGpu);
            auto shCpu = Buffer::Create(impl.allocator, kSHBytes, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
            if (!shCpu) return std::unexpected(shCpu.error());
            state.shCpu = std::move(*shCpu);
        }

        auto lutImg = Image::Create(
            impl.allocator,
            ImageConfig::Texture2D({kLutSize, kLutSize}, VK_FORMAT_R8G8B8A8_UNORM, ImageUsage::Storage | ImageUsage::Sampled)
        );
        if (!lutImg) return std::unexpected(lutImg.error());
        state.payload.brdfLutImage = std::move(*lutImg);
        auto specImg = Image::Create(
            impl.allocator,
            ImageConfig::Cube(kBaseSize, cubeFormat, ImageUsage::Storage | ImageUsage::Sampled, kMipLevels)
        );
        if (!specImg) return std::unexpected(specImg.error());
        state.payload.prefilteredImage  = std::move(*specImg);
        state.payload.prefilteredFormat = cubeFormat;
        state.payload.environmentMode   = environmentMode;
        if (useVisualSky) {
            auto skyImg = Image::Create(
                impl.allocator,
                ImageConfig::Texture2D(
                    {radiance.width, radiance.height}, VK_FORMAT_R32G32B32A32_SFLOAT, ImageUsage::TransferDst | ImageUsage::Sampled
                )
            );
            if (!skyImg) return std::unexpected(skyImg.error());
            state.payload.visualSkyImage = std::move(*skyImg);
        }

        const uint32_t uploadWidth  = hasRadiance ? radiance.width : 1u;
        const uint32_t uploadHeight = hasRadiance ? radiance.height : 1u;
        const uint32_t sourceMipLevels = hasRadiance ? GetMipLevels(uploadWidth, uploadHeight) : 1u;
        const size_t   uploadBytes  = static_cast<size_t>(uploadWidth) * uploadHeight * sizeof(float) * 4u;
        std::array<float, 4> dummy {};
        const float* pixels = hasRadiance ? radiance.rgba : dummy.data();

        // FP32 keeps HDR light energy intact. Linear blits are optional for
        // this format on Vulkan: use the fast image blit when available, and
        // otherwise stage a CPU-built chain instead of issuing invalid blits.
        bool gpuSourceMips = false;
        if (hasRadiance) {
            VkFormatProperties props {};
            vkGetPhysicalDeviceFormatProperties(impl.ctx.Physical(), VK_FORMAT_R32G32B32A32_SFLOAT, &props);
            const auto features = props.optimalTilingFeatures;
            if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0)
                return std::unexpected(EnvironmentBakeError::RadianceFilteringUnsupported);
            constexpr VkFormatFeatureFlags blitFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
            gpuSourceMips = sourceMipLevels > 1 && (features & blitFeatures) == blitFeatures;
        }
        const bool cpuSourceMips = hasRadiance && sourceMipLevels > 1 && !gpuSourceMips;
        if (cpuSourceMips)
            ZHLN::Log("[IBL] FP32 linear blit unsupported; staging the radiance mip chain on the CPU.");

        std::array<size_t, GetMipLevels(kMaxEnvironmentRadianceExtent, kMaxEnvironmentRadianceExtent)> sourceMipOffsets {};
        size_t stagedFloats = static_cast<size_t>(uploadWidth) * uploadHeight * 4u;
        if (cpuSourceMips) {
            for (uint32_t mip = 1; mip < sourceMipLevels; ++mip) {
                sourceMipOffsets[mip] = stagedFloats;
                stagedFloats += static_cast<size_t>(std::max(1u, uploadWidth >> mip)) * std::max(1u, uploadHeight >> mip) * 4u;
            }
        }
        const size_t visualOffsetFloats = stagedFloats;
        if (useVisualSky) stagedFloats += static_cast<size_t>(uploadWidth) * uploadHeight * 4u;
        auto radianceImage = Image::Create(
            impl.allocator,
            ImageConfig::Texture2D(
                {uploadWidth, uploadHeight}, VK_FORMAT_R32G32B32A32_SFLOAT,
                ImageUsage::TransferDst | ImageUsage::Sampled | (gpuSourceMips ? ImageUsage::TransferSrc : ImageUsage::None),
                sourceMipLevels
            )
        );
        if (!radianceImage) return std::unexpected(radianceImage.error());
        ZHLN::defer _([&] { impl.allocator.DestroyImage(*radianceImage); });
        auto staging = Buffer::Create(impl.allocator, stagedFloats * sizeof(float), BufferUsage::TransferSrc, MemoryUsage::CPUOnly);
        if (!staging) return std::unexpected(staging.error());
        ZHLN::defer _([&] { impl.allocator.DestroyBuffer(*staging); });
        {
            auto mapped = staging->Map(impl.allocator);
            if (!mapped) return std::unexpected(mapped.error());
            std::memcpy(mapped->Data(), pixels, uploadBytes);
            if (cpuSourceMips) {
                auto* mipPixels = mapped->As<float>();
                for (uint32_t mip = 1; mip < sourceMipLevels; ++mip) {
                    DownsampleEquirect(mipPixels + sourceMipOffsets[mip - 1], mipPixels + sourceMipOffsets[mip],
                                       std::max(1u, uploadWidth >> (mip - 1)), std::max(1u, uploadHeight >> (mip - 1)),
                                       std::max(1u, uploadWidth >> mip), std::max(1u, uploadHeight >> mip));
                }
            }
            if (useVisualSky) {
                std::memcpy(mapped->As<float>() + visualOffsetFloats, radiance.visualRgba, uploadBytes);
            }
        }

        const BRDFLUTPush lutPush {.width = kLutSize, .height = kLutSize, .sampleCount = 128};
        const IBLBakePush shPush {
            .outAddr      = hasPreparedSH ? 0 : impl.ctx.BufferAddress(state.shGpu.Handle()),
            .sampleCount  = 16384,
            .hasRadiance  = hasRadianceWord,
            .skyZenith    = skyZenith,
            .skyHorizon   = skyHorizon,
            .skyGround    = skyGround,
            .sunDir       = sunDir,
        };

        impl.heapManager.BeginImmediate();

        auto brdfWriteView = state.payload.brdfLutImage.CreateView(impl.ctx.Device());
        if (!brdfWriteView) return std::unexpected(brdfWriteView.error());
        const HeapBlockBase bake2DBlock = impl.heapManager.WriteHeapParameters<Shaders::Bake>(
            impl.ctx, impl.bakeHeapBindings, Vk::Slot<"outTexture">(*brdfWriteView)
        );

        auto radianceWriteView = radianceImage->CreateView(impl.ctx.Device(), {.mipCount = sourceMipLevels});
        if (!radianceWriteView) return std::unexpected(radianceWriteView.error());

        std::array<ImageView, kMipLevels> specMipViews {};
        std::array<HeapBlockBase, kMipLevels> specMipBlocks {};
        for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
            auto mipView = state.payload.prefilteredImage.CreateView(
                impl.ctx.Device(), {.kind = ImageViewKind::Texture2DArray, .baseMip = mip, .mipCount = 1, .layerCount = 6}
            );
            if (!mipView) return std::unexpected(mipView.error());
            specMipViews[mip] = std::move(*mipView);
            specMipBlocks[mip] = impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
                impl.ctx, impl.iblBakeHeapBindings, Vk::Slot<"outTexture">(specMipViews[mip]),
                Vk::Slot<"radianceMap">(*radianceWriteView)
            );
        }
        HeapBlockBase shBlock {};
        if (!hasPreparedSH) {
            shBlock = impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
                impl.ctx, impl.iblBakeHeapBindings, Vk::Slot<"outTexture">(specMipViews[0]), Vk::Slot<"radianceMap">(*radianceWriteView)
            );
        }

        ExecuteImmediate(impl.ctx, impl.graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
            impl.heapManager.BindHeaps(cmd);

            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
                cmd, radianceImage->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, sourceMipLevels
            );
            const VkBufferImageCopy2 region = {
                .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                .pNext             = nullptr,
                .bufferOffset      = 0,
                .bufferRowLength   = 0,
                .bufferImageHeight = 0,
                .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                .imageOffset       = {0, 0, 0},
                .imageExtent       = {uploadWidth, uploadHeight, 1},
            };
            CopyBufferToImage<1>(cmd, staging->Handle(), radianceImage->Handle(), {region});
            if (gpuSourceMips) {
                // The shared helper transitions each generated mip for shader
                // reads. This bake consumes them in COMPUTE, not FRAGMENT.
                GenerateMipmaps(cmd, radianceImage->Handle(), uploadWidth, uploadHeight, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            } else {
                if (cpuSourceMips) {
                    for (uint32_t mip = 1; mip < sourceMipLevels; ++mip) {
                        VkBufferImageCopy2 mipRegion = region;
                        mipRegion.bufferOffset = sourceMipOffsets[mip] * sizeof(float);
                        mipRegion.imageSubresource.mipLevel = mip;
                        mipRegion.imageExtent = {std::max(1u, uploadWidth >> mip), std::max(1u, uploadHeight >> mip), 1u};
                        CopyBufferToImage<1>(cmd, staging->Handle(), radianceImage->Handle(), {mipRegion});
                    }
                }
                ImageBarrier(
cmd, ImageBarrierDesc {
                             .image      = radianceImage->Handle(),
                             .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                             .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                             .base_mip   = 0,
                             .mip_count  = sourceMipLevels,
                         }
                );
            }

            if (useVisualSky) {
                TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, state.payload.visualSkyImage.Handle());
                VkBufferImageCopy2 visualRegion = region;
                visualRegion.bufferOffset = visualOffsetFloats * sizeof(float);
                CopyBufferToImage<1>(cmd, staging->Handle(), state.payload.visualSkyImage.Handle(), {visualRegion});
                ImageBarrier(cmd, ImageBarrierDesc {
                    .image      = state.payload.visualSkyImage.Handle(),
                    .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                    .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dst_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .base_mip   = 0,
                    .mip_count  = 1,
                });
            }

            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.brdfLutImage.Handle());
            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.prefilteredImage.Handle());

            brdfPass->DispatchHeapIndexedThreads<Shaders::Modules::BrdfLutCS>(impl.ctx, cmd, bake2DBlock, kLutSize, kLutSize, 1, lutPush);

            if (!hasPreparedSH) shPass->DispatchHeapIndexedThreads<Shaders::Modules::IblShCS>(impl.ctx, cmd, shBlock, 64, 1, 1, shPush);

            for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                const uint32_t mipSize   = kBaseSize >> mip;
                const float    roughness = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);
                // Source mips filter high-frequency HDR lights according to
                // each GGX ray's PDF. Keep enough samples for broad lobes;
                // the mirror mip reads the source at LOD 0 without filtering.
                const uint32_t sampleCount = mip == 0 ? 1u : (hasRadiance ? (mip >= 3 ? 1024u : 512u) : 32u);
                for (uint32_t face = 0; face < 6; ++face) {
                    const IBLBakePush push {
                        .width       = mipSize,
                        .height      = mipSize,
                        .roughness   = roughness,
                        .face        = face,
                        .sampleCount = sampleCount,
                        .hasRadiance = hasRadianceWord,
                        .skyZenith   = skyZenith,
                        .skyHorizon  = skyHorizon,
                        .skyGround   = skyGround,
                        .sunDir      = sunDir,
                    };
                    specPass->DispatchHeapIndexedThreads<Shaders::Modules::IblSpecularCS>(
                        impl.ctx, cmd, specMipBlocks[mip], mipSize, mipSize, 1, push
                    );
                }
            }

            if (!hasPreparedSH) {
                MemoryBarrier(cmd, BarrierStage::Compute, BarrierAccess::ShaderWrite, BarrierStage::Transfer, BarrierAccess::TransferRead);
                CopyBuffer(cmd, state.shGpu, state.shCpu, kSHBytes);
            }

            TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, state.payload.brdfLutImage.Handle());
            TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, state.payload.prefilteredImage.Handle());
        });

        if (!hasPreparedSH) {
            auto mappedSH = state.shCpu.Map(impl.allocator);
            if (!mappedSH) return std::unexpected(mappedSH.error());
            std::memcpy(state.payload.shCoeffs.data(), mappedSH->Data(), kSHBytes);
        } else {
            // The conditioned map has no tiny emitter for either bake path.
            // Preserve the cooker's exact SH integration of the smooth sky;
            // GPU's 16k-sample estimate can differ at bright subpixel texels.
            for (size_t c = 0; c < state.payload.shCoeffs.size(); ++c) {
                const auto& rgb = radiance.diffuseSH[c];
                state.payload.shCoeffs[c] = JPH::Vec4(rgb[0], rgb[1], rgb[2], 0.0f);
            }
        }

        auto lutView = state.payload.brdfLutImage.CreateView(impl.ctx.Device());
        if (!lutView) return std::unexpected(lutView.error());
        state.payload.brdfLutView = std::move(*lutView);
        auto cubeView = state.payload.prefilteredImage.CreateView(impl.ctx.Device(), {.kind = ImageViewKind::Cube, .mipCount = kMipLevels});
        if (!cubeView) return std::unexpected(cubeView.error());
        state.payload.prefilteredView = std::move(*cubeView);
        if (useVisualSky) {
            auto skyView = state.payload.visualSkyImage.CreateView(impl.ctx.Device());
            if (!skyView) return std::unexpected(skyView.error());
            state.payload.visualSkyView = std::move(*skyView);
        }
        return std::move(state.payload);

    }

  private:
    // Fallback when RGBA32F lacks linear image blits. Integrate the area of
    // each source texel (including the shorter polar rows) into the next mip;
    // unlike a fixed 2x2 average, this also preserves the edge of odd extents.
    static void DownsampleEquirect(const float* src, float* dst, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) noexcept {
        constexpr float pi = 3.141592653589793f;
        for (uint32_t y = 0; y < dstH; ++y) {
            const float top = float(y) * float(srcH) / float(dstH);
            const float bottom = float(y + 1) * float(srcH) / float(dstH);
            for (uint32_t x = 0; x < dstW; ++x) {
                const float left = float(x) * float(srcW) / float(dstW);
                const float right = float(x + 1) * float(srcW) / float(dstW);
                double sum[4] {};
                double weightSum = 0.0;
                for (uint32_t sy = static_cast<uint32_t>(top); sy < srcH && float(sy) < bottom; ++sy) {
                    const float rowOverlap = std::min(float(sy + 1), bottom) - std::max(float(sy), top);
                    const float rowWeight = rowOverlap * std::sin(pi * (float(sy) + 0.5f) / float(srcH));
                    for (uint32_t sx = static_cast<uint32_t>(left); sx < srcW && float(sx) < right; ++sx) {
                        const float colOverlap = std::min(float(sx + 1), right) - std::max(float(sx), left);
                        const double weight = static_cast<double>(rowWeight) * static_cast<double>(colOverlap);
                        const size_t index = (static_cast<size_t>(sy) * srcW + sx) * 4u;
                        for (uint32_t c = 0; c < 4; ++c)
                            sum[c] += static_cast<double>(src[index + c]) * weight;
                        weightSum += weight;
                    }
                }
                const size_t index = (static_cast<size_t>(y) * dstW + x) * 4u;
                for (uint32_t c = 0; c < 4; ++c)
                    dst[index + c] = static_cast<float>(sum[c] / weightSum);
            }
        }
    }

    struct BRDFLUTPush {
        uint32_t width       = 0;
        uint32_t height      = 0;
        uint32_t sampleCount = 0;
    };
    static_assert(GpuAbi::ScenePassPayload<BRDFLUTPush>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

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
    static_assert(GpuAbi::ScenePassPayload<IBLBakePush>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");
    static_assert(sizeof(IBLBakePush) == 96);
};

}
