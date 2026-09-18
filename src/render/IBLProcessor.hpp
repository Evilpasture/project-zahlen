// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include "Resources.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <utility>

namespace ZHLN::Vk {

class IBLProcessor {
  public:
    static auto Bake(RenderContext::Impl& impl, const Components::PostProcessSettingsComponent& sky = {}) -> std::expected<IBLPayload, ZHLN::ErrorCode> {
        constexpr uint32_t kLutSize   = 512;
        constexpr uint32_t kBaseSize  = 256;
        constexpr uint32_t kMipLevels = 6;
        constexpr size_t   kSHBytes   = sizeof(JPH::Vec4) * 9;

        ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips (Slang compute)...");

        const auto requireShader = [](const ZHLN_ShaderDesc& shader) -> std::expected<ZHLN_ShaderDesc, ZHLN::ErrorCode> {
            if (shader.code == nullptr || shader.size == 0) {
                return std::unexpected(ZHLN::Vk::ShaderStageCreationError::ShaderLoadingFailed);
            }
            return shader;
        };

        // Every bake stage is a generated module: the bytes the descriptor checks
        // ran against are the bytes that get loaded, and each module states its own
        // entry point.
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
                    impl.ctx.Device(), specShader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
                )
                    .transform([brdf = std::move(brdf)](DynamicComputePass spec) mutable {
                        return Pipelines {.brdf = std::move(brdf), .spec = std::move(spec), .sh = {}};
                    });
            })
            .and_then([&](Pipelines pipes) -> auto {
                return CreateHeapComputePass(
                    impl.ctx.Device(), shShader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset, impl.pipelineCache.Get()
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
                            .TextureCube(kBaseSize, VK_FORMAT_R8G8B8A8_UNORM, ImageUsage::Storage | ImageUsage::Sampled, kMipLevels)
                            .Build(impl.allocator.Get())
                            .transform([state = std::move(state)](Image specImg) mutable {
                                state.payload.prefilteredImage = std::move(specImg);
                                return std::move(state);
                            });
                    })
                    .transform([pipes = std::move(pipes)](State state) mutable { return std::make_pair(std::move(pipes), std::move(state)); });
            })
            .and_then([&](std::pair<Pipelines, State> packed) -> std::expected<State, ErrorCode> {
                auto [pipes, state] = std::move(packed);

                const BRDFLUTPush lutPush {.width = kLutSize, .height = kLutSize, .sampleCount = 128};
                const IBLBakePush shPush {
                    .outAddr     = impl.ctx.BufferAddress(state.shGpu.Handle()),
                    .sampleCount = 16384,
                    .skyZenith   = sky.skyZenith,
                    .skyHorizon  = sky.skyHorizon,
                    .skyGround   = sky.skyGround,
                    .sunDir      = sunDir,
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

                std::array<VkImageViewCreateInfo, kMipLevels> specMipInfos {};
                std::array<HeapBlockBase, kMipLevels>         specMipBlocks {};
                for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                    specMipInfos[mip] =
                        MakeViewCreateInfo2DArray(state.payload.prefilteredImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 0, 6, VK_IMAGE_ASPECT_COLOR_BIT, 1, mip);
                    specMipBlocks[mip] = impl.heapManager.WriteHeapParameters<Shaders::Bake>(
                        impl.ctx, impl.bakeHeapBindings, Vk::Slot<"outTexture">(ImageWrite {.viewInfo = &specMipInfos[mip]})
                    );
                }

                ExecuteImmediate(impl.ctx, impl.graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
                    impl.heapManager.BindHeaps(cmd);
                    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.brdfLutImage.Handle());
                    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.prefilteredImage.Handle());

                    // The pushed word carries the block's base slot.
                    pipes.brdf.DispatchHeapIndexedThreads(impl.ctx, cmd, bake2DBlock, kLutSize, kLutSize, 1, lutPush);

                    pipes.sh.DispatchHeapIndexedThreads(impl.ctx, cmd, bake2DBlock, 64, 1, 1, shPush);

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
                                .skyZenith   = sky.skyZenith,
                                .skyHorizon  = sky.skyHorizon,
                                .skyGround   = sky.skyGround,
                                .sunDir      = sunDir,
                            };
                            pipes.spec.DispatchHeapIndexedThreads(impl.ctx, cmd, specMipBlocks[mip], mipSize, mipSize, 1, push);
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
                return CreateView<VK_FORMAT_R8G8B8A8_UNORM>(impl.ctx.Device(), state.payload.brdfLutImage.Handle())
                    .transform([state = std::move(state)](ImageView lutView) mutable -> auto {
                        state.payload.brdfLutView = std::move(lutView);
                        state.payload.brdfLutViewInfo =
                            MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                        return std::move(state);
                    });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::ErrorCode> {
                return CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(impl.ctx.Device(), state.payload.prefilteredImage.Handle(), kMipLevels)
                    .transform([state = std::move(state)](ImageView cubeView) mutable -> auto {
                        state.payload.prefilteredView = std::move(cubeView);
                        state.payload.prefilteredViewInfo =
                            MakeViewCreateInfoCube(state.payload.prefilteredImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, kMipLevels);
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

    struct IBLBakePush {
        uint64_t  outAddr     = 0;
        uint32_t  width       = 0;
        uint32_t  height      = 0;
        float     roughness   = 0.0f;
        uint32_t  face        = 0;
        uint32_t  sampleCount = 0;
        uint32_t  _pad        = 0;
        JPH::Vec4 skyZenith   = JPH::Vec4::sZero();
        JPH::Vec4 skyHorizon  = JPH::Vec4::sZero();
        JPH::Vec4 skyGround   = JPH::Vec4::sZero();
        JPH::Vec4 sunDir      = JPH::Vec4::sZero();
    };
    static_assert(sizeof(IBLBakePush) == 96);
};

} // namespace ZHLN::Vk
