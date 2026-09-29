// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include "Resources.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/RadianceMap.hpp>
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <utility>

namespace ZHLN::Vk {

enum class EnvironmentBakeError : uint8_t {
    RadianceTooLarge ZHLN_ANNOTATION(ZHLN::Description<"radiance equirect exceeds the bake size limit"> {}) = 1,
    RadianceUploadFailed ZHLN_ANNOTATION(ZHLN::Description<"radiance equirect could not be staged for the IBL bake"> {}),
    InvalidRadianceData ZHLN_ANNOTATION(ZHLN::Description<"radiance float4 data does not match its extent"> {}),
};

class IBLProcessor {
  public:
    struct RadianceSource {
        const float* rgba;
        uint32_t     width;
        uint32_t     height;
        int          renderSkybox;
    };

    static auto Bake(RenderContext::Impl& impl, const Components::PostProcessSettingsComponent& sky = {}, const RadianceSource& radiance = {})
        -> std::expected<IBLPayload, ZHLN::ErrorCode> {
        constexpr uint32_t kLutSize   = 512;
        constexpr uint32_t kBaseSize  = 256;
        constexpr uint32_t kMipLevels = 6;
        constexpr size_t   kSHBytes   = sizeof(JPH::Vec4) * 9;

        const bool hasRadiance = radiance.rgba != nullptr && radiance.width > 0 && radiance.height > 0;
        if (hasRadiance && (radiance.width > kMaxRadianceExtent || radiance.height > kMaxRadianceExtent)) {
            return std::unexpected(EnvironmentBakeError::RadianceTooLarge);
        }
        const VkFormat cubeFormat = hasRadiance ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const int environmentMode = hasRadiance ? (radiance.renderSkybox != 0 ? 1 : 2) : 0;
        const uint32_t hasRadianceWord = hasRadiance ? 1u : 0u;

        if (hasRadiance) {
            ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips from a {}x{} radiance map...", radiance.width, radiance.height);
        } else {
            ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips (procedural sky)...");
        }

        const auto requireShader = [](const ZHLN_ShaderDesc& shader) -> std::expected<ZHLN_ShaderDesc, ZHLN::ErrorCode> {
            if (shader.code == nullptr || shader.size == 0) {
                return std::unexpected(ZHLN::Vk::ShaderStageCreationError::ShaderLoadingFailed);
            }
            return shader;
        };

        const auto      brdfShader = Vk::CreateShaderDesc<Shaders::Modules::BrdfLutCS>();
        const auto      specShader = Vk::CreateShaderDesc<Shaders::Modules::IblSpecularCS>();
        const auto      shShader   = Vk::CreateShaderDesc<Shaders::Modules::IblShCS>();
        const JPH::Vec4 sunDir     = JPH::Vec4(JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized(), 0.0f);

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

        auto shGpu = Buffer::Create(
            impl.allocator.Get(), kSHBytes,
            BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst | BufferUsage::ShaderDeviceAddress,
            MemoryUsage::GPUOnly
        );
        if (!shGpu) return std::unexpected(shGpu.error());
        state.shGpu = std::move(*shGpu);
        auto shCpu = Buffer::Create(impl.allocator.Get(), kSHBytes, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
        if (!shCpu) return std::unexpected(shCpu.error());
        state.shCpu = std::move(*shCpu);

        auto lutImg = ImageBuilder {}
            .Texture2D(kLutSize, kLutSize, VK_FORMAT_R8G8B8A8_UNORM, ImageUsage::Storage | ImageUsage::Sampled, 1)
            .Build(impl.allocator.Get());
        if (!lutImg) return std::unexpected(lutImg.error());
        state.payload.brdfLutImage = std::move(*lutImg);
        auto specImg = ImageBuilder {}
            .TextureCube(kBaseSize, cubeFormat, ImageUsage::Storage | ImageUsage::Sampled, kMipLevels)
            .Build(impl.allocator.Get());
        if (!specImg) return std::unexpected(specImg.error());
        state.payload.prefilteredImage  = std::move(*specImg);
        state.payload.prefilteredFormat = cubeFormat;
        state.payload.environmentMode   = environmentMode;

        const uint32_t uploadWidth  = hasRadiance ? radiance.width : 1u;
        const uint32_t uploadHeight = hasRadiance ? radiance.height : 1u;
        const size_t   uploadBytes  = static_cast<size_t>(uploadWidth) * uploadHeight * sizeof(float) * 4u;
        std::array<float, 4> dummy {};
        const float* pixels = hasRadiance ? radiance.rgba : dummy.data();

        auto radianceImage = ImageBuilder {}
            .Texture2D(uploadWidth, uploadHeight, VK_FORMAT_R32G32B32A32_SFLOAT,
                       ImageUsage::TransferDst | ImageUsage::Sampled, 1)
            .Build(impl.allocator.Get());
        if (!radianceImage) return std::unexpected(radianceImage.error());
        ZHLN::defer _([&] { impl.allocator.DestroyImage(*radianceImage); });
        auto staging = Buffer::Create(impl.allocator.Get(), uploadBytes, BufferUsage::TransferSrc, MemoryUsage::CPUOnly);
        if (!staging) return std::unexpected(staging.error());
        ZHLN::defer _([&] { impl.allocator.DestroyBuffer(*staging); });
        {
            auto mapped = staging->Map(impl.allocator.Get());
            if (mapped.data == nullptr) return std::unexpected(EnvironmentBakeError::RadianceUploadFailed);
            std::memcpy(mapped.data, pixels, uploadBytes);
        }

        const BRDFLUTPush lutPush {.width = kLutSize, .height = kLutSize, .sampleCount = 128};
        const IBLBakePush shPush {
            .outAddr      = impl.ctx.BufferAddress(state.shGpu.Handle()),
            .sampleCount  = 16384,
            .hasRadiance  = hasRadianceWord,
            .skyZenith    = sky.skyZenith,
            .skyHorizon   = sky.skyHorizon,
            .skyGround    = sky.skyGround,
            .sunDir       = sunDir,
        };

        impl.heapManager.BeginImmediate();

        const auto brdfInfo = MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
        const HeapBlockBase bake2DBlock = impl.heapManager.WriteHeapParameters<Shaders::Bake>(
            impl.ctx, impl.bakeHeapBindings, Vk::Slot<"outTexture">(ImageWrite {brdfInfo})
        );

        const auto radianceInfo =
            MakeViewCreateInfo2D(radianceImage->Handle(), VK_FORMAT_R32G32B32A32_SFLOAT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
        const ImageWrite radianceWrite {radianceInfo};

        std::array<HeapBlockBase, kMipLevels> specMipBlocks {};
        for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
            const auto mipInfo =
                MakeViewCreateInfo2DArray(state.payload.prefilteredImage.Handle(), cubeFormat, 0, 6, VK_IMAGE_ASPECT_COLOR_BIT, 1, mip);
            specMipBlocks[mip] = impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
                impl.ctx, impl.iblBakeHeapBindings, Vk::Slot<"outTexture">(ImageWrite {mipInfo}),
                Vk::Slot<"radianceMap">(radianceWrite)
            );
        }
        const auto shMipInfo =
            MakeViewCreateInfo2DArray(state.payload.prefilteredImage.Handle(), cubeFormat, 0, 6, VK_IMAGE_ASPECT_COLOR_BIT, 1, 0);
        const HeapBlockBase shBlock = impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
            impl.ctx, impl.iblBakeHeapBindings, Vk::Slot<"outTexture">(ImageWrite {shMipInfo}), Vk::Slot<"radianceMap">(radianceWrite)
        );

        ExecuteImmediate(impl.ctx, impl.graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
            impl.heapManager.BindHeaps(cmd);

            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
                cmd, radianceImage->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
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
            ImageBarrier(
                cmd, ZHLN_ImageBarrierDesc {
                         .image      = radianceImage->Handle(),
                         .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                         .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                         .base_mip   = 0,
                         .mip_count  = 1,
                     }
            );

            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.brdfLutImage.Handle());
            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.prefilteredImage.Handle());

            brdfPass->DispatchHeapIndexedThreads<Shaders::Modules::BrdfLutCS>(impl.ctx, cmd, bake2DBlock, kLutSize, kLutSize, 1, lutPush);

            shPass->DispatchHeapIndexedThreads<Shaders::Modules::IblShCS>(impl.ctx, cmd, shBlock, 64, 1, 1, shPush);

            for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                const uint32_t mipSize   = kBaseSize >> mip;
                const float    roughness = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);
                // The HDR source has only mip 0. At 32 samples a tiny
                // studio light is missed by most texels and becomes a
                // few bright speckles in the others. Spend the extra
                // samples once when baking HDR; the smaller rough mips
                // need more samples to resolve the whole hemisphere.
                const uint32_t sampleCount = mip == 0 ? 1u : (hasRadiance ? (mip >= 3 ? 1024u : 512u) : 32u);
                for (uint32_t face = 0; face < 6; ++face) {
                    const IBLBakePush push {
                        .width       = mipSize,
                        .height      = mipSize,
                        .roughness   = roughness,
                        .face        = face,
                        .sampleCount = sampleCount,
                        .hasRadiance = hasRadianceWord,
                        .skyZenith   = sky.skyZenith,
                        .skyHorizon  = sky.skyHorizon,
                        .skyGround   = sky.skyGround,
                        .sunDir      = sunDir,
                    };
                    specPass->DispatchHeapIndexedThreads<Shaders::Modules::IblSpecularCS>(
                        impl.ctx, cmd, specMipBlocks[mip], mipSize, mipSize, 1, push
                    );
                }
            }

            MemoryBarrier(cmd, BarrierStage::Compute, BarrierAccess::ShaderWrite, BarrierStage::Transfer, BarrierAccess::TransferRead);
            CopyBuffer(cmd, state.shGpu, state.shCpu, kSHBytes);

            TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, state.payload.brdfLutImage.Handle());
            TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, state.payload.prefilteredImage.Handle());
        });

        auto mappedSH = state.shCpu.Map(impl.allocator.Get());
        if (mappedSH.data == nullptr) return std::unexpected(StagingError::MemoryMappingFailed);
        std::memcpy(state.payload.shCoeffs.data(), mappedSH.data, kSHBytes);

        const auto lutInfo = MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
        auto lutView = ImageView::Create(impl.ctx.Device(), lutInfo);
        if (!lutView) return std::unexpected(lutView.error());
        state.payload.brdfLutView = std::move(*lutView);
        const auto cubeInfo = MakeViewCreateInfoCube(state.payload.prefilteredImage.Handle(), cubeFormat, kMipLevels);
        auto cubeView = ImageView::Create(impl.ctx.Device(), cubeInfo);
        if (!cubeView) return std::unexpected(cubeView.error());
        state.payload.prefilteredView = std::move(*cubeView);
        return std::move(state.payload);

    }

  private:
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
