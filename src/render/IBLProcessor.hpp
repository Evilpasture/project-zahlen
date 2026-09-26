// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include "Resources.hpp"
#include <Zahlen/Components.hpp>
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
};

// Already-decoded RGBA32F. A null pointer (or a zero extent) bakes the
// procedural sky. The equirect is destroyed after the immediate submit;
// the skybox samples the prefiltered cube's mip 0.
//
// Not nested in IBLProcessor. Clang will not value-initialize a nested
// aggregate from a default argument of the enclosing class: the default
// member initializers are not parsed until that class is complete
// ("needed within definition of enclosing class outside of member functions").
struct RadianceSource {
    const float* rgba         = nullptr;
    uint32_t     width        = 0;
    uint32_t     height       = 0;
    int          renderSkybox = 0;
};

class IBLProcessor {
  public:
    using RadianceSource = ZHLN::Vk::RadianceSource;

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
        // HDR values do not fit in the procedural UNORM cube (the sun disk
        // already clamped). 16F keeps the range; the procedural path stays
        // UNORM so existing captures do not shift from an unclamped disk.
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

        // Every bake stage is a generated module: the bytes the descriptor checks
        // ran against are the bytes that get loaded, and each module states its own
        // entry point. Specular and SH share iblBakeHeapBindings (radiance +
        // storage image). BRDF stays on the procedural bake table.
        const auto      brdfShader = Vk::CreateShaderDesc<Shaders::Modules::BrdfLutCS>();
        const auto      specShader = Vk::CreateShaderDesc<Shaders::Modules::IblSpecularCS>();
        const auto      shShader   = Vk::CreateShaderDesc<Shaders::Modules::IblShCS>();
        const JPH::Vec4 sunDir     = JPH::Vec4(JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized(), 0.0f);

        struct Pipelines {
            DynamicComputePass brdf;
            DynamicComputePass spec;
            DynamicComputePass sh;
        };

        struct State {
            Buffer     shGpu;
            Buffer     shCpu;
            IBLPayload payload;
        };

        return requireShader(brdfShader)
            .and_then([&](auto) -> auto { return requireShader(specShader); })
            .and_then([&](auto) -> auto { return requireShader(shShader); })
            .and_then([&](auto) -> auto {
                return CreateHeapComputePass(
                    impl.ctx.Device(), brdfShader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
                );
            })
            .and_then([&](DynamicComputePass brdf) -> auto {
                return CreateHeapComputePass(
                    impl.ctx.Device(), specShader, impl.iblBakeHeapBindings.GetInfo(), impl.iblBakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
                )
                    .transform([brdf = std::move(brdf)](DynamicComputePass spec) mutable {
                        return Pipelines {.brdf = std::move(brdf), .spec = std::move(spec), .sh = {}};
                    });
            })
            .and_then([&](Pipelines pipes) -> auto {
                return CreateHeapComputePass(
                    impl.ctx.Device(), shShader, impl.iblBakeHeapBindings.GetInfo(), impl.iblBakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
                )
                    .transform([pipes = std::move(pipes)](DynamicComputePass sh) mutable {
                        pipes.sh = std::move(sh);
                        return std::move(pipes);
                    });
            })
            .and_then([&](Pipelines pipes) -> std::expected<std::pair<Pipelines, State>, ErrorCode> {
                return Buffer::Create(
                           impl.allocator.Get(), kSHBytes,
                           BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst |
                               BufferUsage::ShaderDeviceAddress,
                           MemoryUsage::GPUOnly
                )
                    .and_then([&](Buffer shGpu) -> auto {
                        return Buffer::Create(impl.allocator.Get(), kSHBytes, BufferUsage::TransferDst, MemoryUsage::GPUToCPU)
                            .transform([shGpu = std::move(shGpu)](Buffer shCpu) mutable {
                                return State {.shGpu = std::move(shGpu), .shCpu = std::move(shCpu)};
                            });
                    })
                    .and_then([&](State state) -> auto {
                        return ImageBuilder {}
                            .Texture2D(kLutSize, kLutSize, VK_FORMAT_R8G8B8A8_UNORM, ImageUsage::Storage | ImageUsage::Sampled, 1)
                            .Build(impl.allocator.Get())
                            .transform([state = std::move(state)](Image lutImg) mutable {
                                state.payload.brdfLutImage = std::move(lutImg);
                                return std::move(state);
                            });
                    })
                    .and_then([&](State state) -> auto {
                        return ImageBuilder {}
                            .TextureCube(kBaseSize, cubeFormat, ImageUsage::Storage | ImageUsage::Sampled, kMipLevels)
                            .Build(impl.allocator.Get())
                            .transform([state = std::move(state), cubeFormat, environmentMode](Image specImg) mutable {
                                state.payload.prefilteredImage  = std::move(specImg);
                                state.payload.prefilteredFormat = cubeFormat;
                                state.payload.environmentMode   = environmentMode;
                                return std::move(state);
                            });
                    })
                    .transform([pipes = std::move(pipes)](State state) mutable { return std::make_pair(std::move(pipes), std::move(state)); });
            })
            .and_then([&](std::pair<Pipelines, State> packed) -> std::expected<State, ErrorCode> {
                auto [pipes, state] = std::move(packed);

                const uint32_t uploadWidth  = hasRadiance ? radiance.width : 1u;
                const uint32_t uploadHeight = hasRadiance ? radiance.height : 1u;
                const size_t   uploadBytes  = static_cast<size_t>(uploadWidth) * uploadHeight * sizeof(float) * 4u;
                std::array<float, 4> dummy {};
                const float* pixels = hasRadiance ? radiance.rgba : dummy.data();

                auto radianceImage = ImageBuilder {}
                                         .Texture2D(
                                             uploadWidth, uploadHeight, VK_FORMAT_R32G32B32A32_SFLOAT,
                                             ImageUsage::TransferDst | ImageUsage::Sampled, 1
                                         )
                                         .Build(impl.allocator.Get());
                if (!radianceImage) {
                    return std::unexpected(radianceImage.error());
                }
                auto staging = Buffer::Create(impl.allocator.Get(), uploadBytes, BufferUsage::TransferSrc, MemoryUsage::CPUOnly);
                if (!staging) {
                    return std::unexpected(staging.error());
                }
                {
                    auto mapped = staging->Map();
                    if (mapped.data == nullptr) {
                        return std::unexpected(EnvironmentBakeError::RadianceUploadFailed);
                    }
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

                // One dispatched command buffer, one block per dispatch: the
                // LUT bakes share a block (the shader bound to outAddr does not
                // sample the texture it is bound to), every specular mip gets its
                // own because all of them are recorded before the submission
                // retires. BeginImmediate rewinds the bake partition, and
                // ExecuteImmediate waits on the fence, so no earlier bake can
                // still be reading what this one overwrites.
                impl.heapManager.BeginImmediate();

                const auto brdfInfo = MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                const HeapBlockBase bake2DBlock = impl.heapManager.WriteHeapParameters<Shaders::Bake>(
                    impl.ctx, impl.bakeHeapBindings, Vk::Slot<"outTexture">(ImageWrite {.viewInfo = &brdfInfo})
                );

                const auto radianceInfo =
                    MakeViewCreateInfo2D(radianceImage->Handle(), VK_FORMAT_R32G32B32A32_SFLOAT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                const ImageWrite radianceWrite {.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .viewInfo = &radianceInfo};

                std::array<VkImageViewCreateInfo, kMipLevels> specMipInfos {};
                std::array<HeapBlockBase, kMipLevels>         specMipBlocks {};
                for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                    specMipInfos[mip] =
                        MakeViewCreateInfo2DArray(state.payload.prefilteredImage.Handle(), cubeFormat, 0, 6, VK_IMAGE_ASPECT_COLOR_BIT, 1, mip);
                    specMipBlocks[mip] = impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
                        impl.ctx, impl.iblBakeHeapBindings, Vk::Slot<"outTexture">(ImageWrite {.viewInfo = &specMipInfos[mip]}),
                        Vk::Slot<"radianceMap">(radianceWrite)
                    );
                }
                // SH writes coefficients through outAddr and does not declare
                // outTexture. The set still names it because SpecularMain does;
                // the extra descriptor is unused by this dispatch.
                const HeapBlockBase shBlock = impl.heapManager.WriteHeapParameters<Shaders::IblBake>(
                    impl.ctx, impl.iblBakeHeapBindings, Vk::Slot<"outTexture">(ImageWrite {.viewInfo = &specMipInfos[0]}),
                    Vk::Slot<"radianceMap">(radianceWrite)
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
                    // TransitionLayout's SHADER_READ destination stage is the
                    // fragment stage only. This bake samples from compute, so
                    // the copy has to be visible there or the prefilter reads
                    // the image before the upload lands.
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

                    // The pushed word carries the block's base slot.
                    pipes.brdf.DispatchHeapIndexedThreads<Shaders::Modules::BrdfLutCS>(impl.ctx, cmd, bake2DBlock, kLutSize, kLutSize, 1, lutPush);

                    pipes.sh.DispatchHeapIndexedThreads<Shaders::Modules::IblShCS>(impl.ctx, cmd, shBlock, 64, 1, 1, shPush);

                    for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                        const uint32_t mipSize   = kBaseSize >> mip;
                        const float    roughness = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);
                        for (uint32_t face = 0; face < 6; ++face) {
                            const IBLBakePush push {
                                .width       = mipSize,
                                .height      = mipSize,
                                .roughness   = roughness,
                                .face        = face,
                                .sampleCount = roughness == 0.0f ? 1u : 32u,
                                .hasRadiance = hasRadianceWord,
                                .skyZenith   = sky.skyZenith,
                                .skyHorizon  = sky.skyHorizon,
                                .skyGround   = sky.skyGround,
                                .sunDir      = sunDir,
                            };
                            pipes.spec.DispatchHeapIndexedThreads<Shaders::Modules::IblSpecularCS>(
                                impl.ctx, cmd, specMipBlocks[mip], mipSize, mipSize, 1, push
                            );
                        }
                    }

                    MemoryBarrier(cmd, BarrierStage::Compute, BarrierAccess::ShaderWrite, BarrierStage::Transfer, BarrierAccess::TransferRead);
                    CopyBuffer(cmd, state.shGpu, state.shCpu, kSHBytes);

                    TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, state.payload.brdfLutImage.Handle());
                    TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, state.payload.prefilteredImage.Handle());
                });

                auto mappedSH = state.shCpu.Map();
                if (mappedSH.data == nullptr) {
                    return std::unexpected(ZHLN::Vk::StagingError::MemoryMappingFailed);
                }
                std::memcpy(state.payload.shCoeffs.data(), mappedSH.data, kSHBytes);
                return std::move(state);
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::ErrorCode> {
                const auto info = MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                return CreateView(impl.ctx.Device(), info).transform([state = std::move(state), info](ImageView lutView) mutable -> auto {
                    state.payload.brdfLutView     = std::move(lutView);
                    state.payload.brdfLutViewInfo = info;
                    return std::move(state);
                });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::ErrorCode> {
                const auto info = MakeViewCreateInfoCube(state.payload.prefilteredImage.Handle(), state.payload.prefilteredFormat, kMipLevels);
                return CreateView(impl.ctx.Device(), info).transform([state = std::move(state), info](ImageView cubeView) mutable -> auto {
                    state.payload.prefilteredView     = std::move(cubeView);
                    state.payload.prefilteredViewInfo = info;
                    return std::move(state);
                });
            })
            .transform([](State state) -> auto { return std::move(state.payload); });
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

} // namespace ZHLN::Vk
