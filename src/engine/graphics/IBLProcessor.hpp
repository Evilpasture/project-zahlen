// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <StagingContext.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstddef>
#include <cstring>
#include <expected>
#include <utility>

namespace ZHLN::Vk {

class IBLProcessor {
  public:
    static auto Bake(RenderContext::Impl& impl, StagingContext& staging, const Components::PostProcessSettingsComponent& sky = {})
        -> std::expected<IBLPayload, ZHLN::Error> {
        using enum ZHLN::Resource::ShaderID;
        constexpr uint32_t kLutSize   = 512;
        constexpr uint32_t kBaseSize  = 256;
        constexpr uint32_t kMipLevels = 6;
        constexpr size_t   kSHBytes   = sizeof(JPH::Vec4) * 9;
        constexpr size_t   kLutBytes  = static_cast<size_t>(kLutSize) * kLutSize * 4;

        ZHLN::Log("[IBL] Baking BRDF LUT / SH / specular mips (Slang compute)...");

        const auto requireShader = [](const ZHLN_ShaderDesc& shader) -> std::expected<ZHLN_ShaderDesc, ZHLN::Error> {
            if (shader.code == nullptr || shader.size == 0) {
                return std::unexpected(ZHLN::ShaderStageCreationError::ShaderLoadingFailed);
            }
            return shader;
        };

        const auto brdfShader = CreateShaderDesc(Resource::GetShaderProgram(BRDFLUTComp).vertex, "CSMain");
        const auto specShader = CreateShaderDesc(Resource::GetShaderProgram(IBLSpecularComp).vertex, "SpecularMain");
        const auto shShader   = CreateShaderDesc(Resource::GetShaderProgram(IBLSHComp).vertex, "SHMain");

        const IBLBakePush skyPush = MakeSkyPush(sky);

        struct Pipelines {
            ComputePass brdf;
            ComputePass spec;
            ComputePass sh;
        };

        struct State {
            Buffer     lutBuf;
            Buffer     shGpu;
            Buffer     shCpu;
            Buffer     specBuf;
            IBLPayload payload;
        };

        return requireShader(brdfShader)
            .and_then([&](auto) { return requireShader(specShader); })
            .and_then([&](auto) { return requireShader(shShader); })
            .and_then([&](auto) { return impl.BuildOneShotCompute(brdfShader); })
            .and_then([&](ComputePass brdf) {
                return impl.BuildOneShotCompute(specShader).transform([brdf = std::move(brdf)](ComputePass spec) mutable {
                    return Pipelines {.brdf = std::move(brdf), .spec = std::move(spec), .sh = {}};
                });
            })
            .and_then([&](Pipelines pipes) {
                return impl.BuildOneShotCompute(shShader).transform([pipes = std::move(pipes)](ComputePass sh) mutable {
                    pipes.sh = std::move(sh);
                    return std::move(pipes);
                });
            })
            .and_then([&](Pipelines pipes) -> std::expected<std::pair<Pipelines, State>, ZHLN::Error> {
                return Buffer::Create(
                           impl.allocator.Get(), kLutBytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VMA_MEMORY_USAGE_GPU_ONLY
                )
                    .and_then([&](Buffer lutBuf) {
                        return Buffer::Create(
                                   impl.allocator.Get(), kSHBytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VMA_MEMORY_USAGE_GPU_ONLY
                        )
                            .transform([lutBuf = std::move(lutBuf)](Buffer shGpu) mutable {
                                return State {.lutBuf = std::move(lutBuf), .shGpu = std::move(shGpu)};
                            });
                    })
                    .and_then([&](State state) {
                        return Buffer::Create(impl.allocator.Get(), kSHBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
                            .transform([state = std::move(state)](Buffer shCpu) mutable {
                                state.shCpu = std::move(shCpu);
                                return std::move(state);
                            });
                    })
                    .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                        size_t totalBytes = 0;
                        for (uint32_t m = 0; m < kMipLevels; ++m) {
                            const uint32_t s = kBaseSize >> m;
                            totalBytes += static_cast<size_t>(s) * s * 4 * 6;
                        }
                        return Buffer::Create(
                                   impl.allocator.Get(), totalBytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VMA_MEMORY_USAGE_GPU_ONLY
                        )
                            .transform([state = std::move(state)](Buffer specBuf) mutable {
                                state.specBuf = std::move(specBuf);
                                return std::move(state);
                            });
                    })
                    .transform([pipes = std::move(pipes)](State state) mutable { return std::make_pair(std::move(pipes), std::move(state)); });
            })
            .and_then([&](std::pair<Pipelines, State> packed) -> std::expected<State, ZHLN::Error> {
                auto [pipes, state] = std::move(packed);

                const VkDeviceAddress specAddr = impl.ctx.BufferAddress(state.specBuf.Handle());
                const BRDFLUTPush     lutPush {
                    .outAddr = impl.ctx.BufferAddress(state.lutBuf.Handle()), .width = kLutSize, .height = kLutSize, .sampleCount = 128
                };
                IBLBakePush shPush     = skyPush;
                shPush.outAddr         = impl.ctx.BufferAddress(state.shGpu.Handle());
                shPush.sampleCount     = 16384;

                ExecuteImmediate(impl.ctx, impl.graphicsCmdRing, [&](VkCommandBuffer cmd) {
                    pipes.brdf.Bind(cmd);
                    PushData(impl.ctx, cmd, 0, lutPush);
                    pipes.brdf.DispatchThreads(cmd, kLutSize, kLutSize, 1);

                    pipes.sh.Bind(cmd);
                    PushData(impl.ctx, cmd, 0, shPush);
                    pipes.sh.DispatchThreads(cmd, 64, 1, 1);

                    pipes.spec.Bind(cmd);
                    size_t byteOffset = 0;
                    for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                        const uint32_t mipSize   = kBaseSize >> mip;
                        const float    roughness = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);
                        const auto     faceBytes = static_cast<size_t>(mipSize) * mipSize * 4;
                        for (uint32_t face = 0; face < 6; ++face) {
                            IBLBakePush push = skyPush;
                            push.outAddr     = specAddr + byteOffset;
                            push.width       = mipSize;
                            push.height      = mipSize;
                            push.roughness   = roughness;
                            push.face        = face;
                            push.sampleCount = roughness == 0.0f ? 1u : 32u;
                            PushData(impl.ctx, cmd, 0, push);
                            pipes.spec.DispatchThreads(cmd, mipSize, mipSize, 1);
                            byteOffset += faceBytes;
                        }
                    }

                    MemoryBarrier(
                        cmd, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                              .dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT}
                    );
                    CopyBuffer(cmd, state.shGpu, state.shCpu, kSHBytes);
                });

                auto mappedSH = state.shCpu.Map();
                if (mappedSH.data == nullptr) {
                    return std::unexpected(ZHLN::RenderInitError::SubsystemAllocationFailed);
                }
                std::memcpy(state.payload.shCoeffs.data(), mappedSH.data, kSHBytes);
                return std::move(state);
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                const VkImageCreateInfo lutInfo = {
                    .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .pNext                 = nullptr,
                    .flags                 = 0,
                    .imageType             = VK_IMAGE_TYPE_2D,
                    .format                = VK_FORMAT_R8G8B8A8_UNORM,
                    .extent                = {.width = kLutSize, .height = kLutSize, .depth = 1},
                    .mipLevels             = 1,
                    .arrayLayers           = 1,
                    .samples               = VK_SAMPLE_COUNT_1_BIT,
                    .tiling                = VK_IMAGE_TILING_OPTIMAL,
                    .usage                 = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices   = nullptr,
                    .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED
                };
                return Image::Create(impl.allocator.Get(), lutInfo, VMA_MEMORY_USAGE_GPU_ONLY).transform([state = std::move(state)](Image lutImg) mutable {
                    state.payload.brdfLutImage = std::move(lutImg);
                    return std::move(state);
                });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                staging.UploadImage2DBuffer(state.payload.brdfLutImage.Handle(), kLutSize, kLutSize, 1, state.lutBuf.Handle(), 0);
                staging.AddBuffer(std::move(state.lutBuf));
                return CreateView<VK_FORMAT_R8G8B8A8_UNORM>(impl.ctx.Device(), state.payload.brdfLutImage.Handle())
                    .transform([state = std::move(state)](ImageView lutView) mutable {
                        state.payload.brdfLutView     = std::move(lutView);
                        state.payload.brdfLutViewInfo = MakeViewCreateInfo2D(state.payload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                        return std::move(state);
                    });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                const VkImageCreateInfo specInfo = {
                    .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .pNext                 = nullptr,
                    .flags                 = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                    .imageType             = VK_IMAGE_TYPE_2D,
                    .format                = VK_FORMAT_R8G8B8A8_UNORM,
                    .extent                = {.width = kBaseSize, .height = kBaseSize, .depth = 1},
                    .mipLevels             = kMipLevels,
                    .arrayLayers           = 6,
                    .samples               = VK_SAMPLE_COUNT_1_BIT,
                    .tiling                = VK_IMAGE_TILING_OPTIMAL,
                    .usage                 = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices   = nullptr,
                    .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED
                };
                return Image::Create(impl.allocator.Get(), specInfo, VMA_MEMORY_USAGE_GPU_ONLY).transform([state = std::move(state)](Image specImg) mutable {
                    state.payload.prefilteredImage = std::move(specImg);
                    return std::move(state);
                });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                staging.UploadPrefilteredCubeMap(state.payload.prefilteredImage.Handle(), state.specBuf.Handle(), kBaseSize, kMipLevels);
                staging.AddBuffer(std::move(state.specBuf));
                return CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(impl.ctx.Device(), state.payload.prefilteredImage.Handle(), kMipLevels)
                    .transform([state = std::move(state)](ImageView cubeView) mutable {
                        state.payload.prefilteredView     = std::move(cubeView);
                        state.payload.prefilteredViewInfo = MakeViewCreateInfoCube(state.payload.prefilteredImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, kMipLevels);
                        return std::move(state);
                    });
            })
            .transform([](State state) { return std::move(state.payload); });
    }

  private:
    struct BRDFLUTPush {
        uint64_t outAddr     = 0;
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

    static auto MakeSkyPush(const Components::PostProcessSettingsComponent& sky) noexcept -> IBLBakePush {
        const JPH::Vec3 sun = JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized();
        return IBLBakePush {
            .skyZenith  = sky.skyZenith,
            .skyHorizon = sky.skyHorizon,
            .skyGround  = sky.skyGround,
            .sunDir     = JPH::Vec4(sun, 0.0f),
        };
    }
};

} // namespace ZHLN::Vk
