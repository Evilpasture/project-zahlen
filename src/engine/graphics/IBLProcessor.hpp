// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
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
    static auto Bake(RenderContext::Impl& impl, const Components::PostProcessSettingsComponent& sky = {}) -> std::expected<IBLPayload, ZHLN::Error> {
        using enum ZHLN::Resource::ShaderID;
        constexpr uint32_t kLutSize   = 512;
        constexpr uint32_t kBaseSize  = 256;
        constexpr uint32_t kMipLevels = 6;
        constexpr size_t   kSHBytes   = sizeof(JPH::Vec4) * 9;

        ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips (Slang compute)...");

        const auto requireShader = [](const ZHLN_ShaderDesc& shader) -> std::expected<ZHLN_ShaderDesc, ZHLN::Error> {
            if (shader.code == nullptr || shader.size == 0) {
                return std::unexpected(ZHLN::Vk::ShaderStageCreationError::ShaderLoadingFailed);
            }
            return shader;
        };

        const auto      brdfShader = CreateShaderDesc(Resource::GetShaderProgram(BRDFLUTComp).vertex, "CSMain");
        const auto      specShader = CreateShaderDesc(Resource::GetShaderProgram(IBLSpecularComp).vertex, "SpecularMain");
        const auto      shShader   = CreateShaderDesc(Resource::GetShaderProgram(IBLSHComp).vertex, "SHMain");
        const JPH::Vec4 sunDir     = JPH::Vec4(JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized(), 0.0f);

        struct Pipelines {
            ComputePass brdf;
            ComputePass spec;
            ComputePass sh;
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
                return CreateHeapComputePass(impl.ctx.Device(), brdfShader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset);
            })
            .and_then([&](ComputePass brdf) -> auto {
                return CreateHeapComputePass(impl.ctx.Device(), specShader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset)
                    .transform([brdf = std::move(brdf)](ComputePass spec) mutable {
                        return Pipelines {.brdf = std::move(brdf), .spec = std::move(spec), .sh = {}};
                    });
            })
            .and_then([&](Pipelines pipes) -> auto {
                return CreateHeapComputePass(impl.ctx.Device(), shShader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset)
                    .transform([pipes = std::move(pipes)](ComputePass sh) mutable {
                        pipes.sh = std::move(sh);
                        return std::move(pipes);
                    });
            })
            .and_then([&](Pipelines pipes) -> std::expected<std::pair<Pipelines, State>, Error> {
                return Buffer::Create(
                           impl.allocator.Get(), kSHBytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VMA_MEMORY_USAGE_GPU_ONLY
                )
                    .and_then([&](Buffer shGpu) -> auto {
                        return Buffer::Create(impl.allocator.Get(), kSHBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
                            .transform([shGpu = std::move(shGpu)](Buffer shCpu) mutable {
                                return State {.shGpu = std::move(shGpu), .shCpu = std::move(shCpu)};
                            });
                    })
                    .and_then([&](State state) -> auto {
                        return ImageBuilder {}
                            .Texture2D(kLutSize, kLutSize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1)
                            .Build(impl.allocator.Get())
                            .transform([state = std::move(state)](Image lutImg) mutable {
                                state.payload.brdfLutImage = std::move(lutImg);
                                return std::move(state);
                            });
                    })
                    .and_then([&](State state) -> auto {
                        return ImageBuilder {}
                            .TextureCube(kBaseSize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, kMipLevels)
                            .Build(impl.allocator.Get())
                            .transform([state = std::move(state)](Image specImg) mutable {
                                state.payload.prefilteredImage = std::move(specImg);
                                return std::move(state);
                            });
                    })
                    .transform([pipes = std::move(pipes)](State state) mutable { return std::make_pair(std::move(pipes), std::move(state)); });
            })
            .and_then([&](std::pair<Pipelines, State> packed) -> std::expected<State, Error> {
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

                const auto brdfInfo = MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                impl.heapManager.WriteBindings(impl.ctx, impl.bakeHeapBindings, RenderContext::Impl::kBake2DHeapIndex, ImageWrite {.viewInfo = &brdfInfo});

                std::array<VkImageViewCreateInfo, kMipLevels> specMipInfos {};
                for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                    specMipInfos[mip] =
                        MakeViewCreateInfo2DArray(state.payload.prefilteredImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 0, 6, VK_IMAGE_ASPECT_COLOR_BIT, 1, mip);
                    impl.heapManager.WriteBindings(
                        impl.ctx, impl.bakeHeapBindings, RenderContext::Impl::kBakeSpecHeapIndex0 + mip, ImageWrite {.viewInfo = &specMipInfos[mip]}
                    );
                }

                ExecuteImmediate(impl.ctx, impl.graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
                    impl.heapManager.BindHeaps(cmd);
                    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.brdfLutImage.Handle());
                    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, state.payload.prefilteredImage.Handle());

                    pipes.brdf.DispatchHeapIndexedThreads(impl.ctx, cmd, RenderContext::Impl::kBake2DHeapIndex, kLutSize, kLutSize, 1, lutPush);

                    pipes.sh.DispatchHeapIndexedThreads(impl.ctx, cmd, RenderContext::Impl::kBake2DHeapIndex, 64, 1, 1, shPush);

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
                            pipes.spec.DispatchHeapIndexedThreads(impl.ctx, cmd, RenderContext::Impl::kBakeSpecHeapIndex0 + mip, mipSize, mipSize, 1, push);
                        }
                    }

                    MemoryBarrier(
                        cmd, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                              .dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT}
                    );
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
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                return CreateView<VK_FORMAT_R8G8B8A8_UNORM>(impl.ctx.Device(), state.payload.brdfLutImage.Handle())
                    .transform([state = std::move(state)](ImageView lutView) mutable -> auto {
                        state.payload.brdfLutView = std::move(lutView);
                        state.payload.brdfLutViewInfo =
                            MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                        return std::move(state);
                    });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
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
