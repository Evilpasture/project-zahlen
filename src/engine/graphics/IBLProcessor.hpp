// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <StagingContext.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstddef>
#include <cstring>
#include <expected>
#include <utility>

namespace ZHLN::Vk {

class IBLProcessor {
  public:
    static auto Bake(RenderContext::Impl& impl, StagingContext& staging) -> std::expected<IBLPayload, ZHLN::Error> {
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
            .and_then([&](auto) -> std::expected<Buffer, ZHLN::Error> {
                return Buffer::Create(
                    impl.allocator.Get(), kLutBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .and_then([&](Buffer lutBuf) {
                const BRDFLUTPush lutPush {
                    .outAddr = impl.ctx.BufferAddress(lutBuf.Handle()), .width = kLutSize, .height = kLutSize, .sampleCount = 128
                };
                return impl.DispatchOneShotCompute(brdfShader, &lutPush, sizeof(lutPush), kLutSize, kLutSize, 1).transform([lutBuf = std::move(lutBuf)]() mutable {
                    return State {.lutBuf = std::move(lutBuf)};
                });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                return Buffer::Create(
                           impl.allocator.Get(), kSHBytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VMA_MEMORY_USAGE_GPU_ONLY
                )
                    .transform([state = std::move(state)](Buffer shGpu) mutable {
                        state.shGpu = std::move(shGpu);
                        return std::move(state);
                    });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                return Buffer::Create(impl.allocator.Get(), kSHBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
                    .transform([state = std::move(state)](Buffer shCpu) mutable {
                        state.shCpu = std::move(shCpu);
                        return std::move(state);
                    });
            })
            .and_then([&](State state) {
                const IBLBakePush shPush {.outAddr = impl.ctx.BufferAddress(state.shGpu.Handle()), .sampleCount = 16384};
                return impl.DispatchOneShotCompute(shShader, &shPush, sizeof(shPush), 64, 1, 1).transform([state = std::move(state)]() mutable {
                    return std::move(state);
                });
            })
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                ExecuteImmediate(impl.ctx, impl.graphicsCmdRing, [&](VkCommandBuffer cmd) { CopyBuffer(cmd, state.shGpu, state.shCpu, kSHBytes); });
                auto mappedSH = state.shCpu.Map();
                if (mappedSH.data == nullptr) {
                    return std::unexpected(ZHLN::RenderInitError::SubsystemAllocationFailed);
                }
                std::memcpy(state.payload.shCoeffs.data(), mappedSH.data, kSHBytes);
                return state;
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
            .and_then([&](State state) -> std::expected<State, ZHLN::Error> {
                const VkDeviceAddress            specAddr   = impl.ctx.BufferAddress(state.specBuf.Handle());
                size_t                           byteOffset = 0;
                std::expected<void, ZHLN::Error> dispatched {};
                for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                    const uint32_t mipSize   = kBaseSize >> mip;
                    const float    roughness = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);
                    const auto     faceBytes = static_cast<size_t>(mipSize) * mipSize * 4;
                    for (uint32_t face = 0; face < 6; ++face) {
                        dispatched = std::move(dispatched).and_then([&, specAddr, byteOffset, mipSize, roughness, face]() {
                            const IBLBakePush push {
                                .outAddr     = specAddr + byteOffset,
                                .width       = mipSize,
                                .height      = mipSize,
                                .roughness   = roughness,
                                .face        = face,
                                .sampleCount = roughness == 0.0f ? 1u : 32u
                            };
                            return impl.DispatchOneShotCompute(specShader, &push, sizeof(push), mipSize, mipSize, 1);
                        });
                        byteOffset += faceBytes;
                    }
                }
                return std::move(dispatched).transform([state = std::move(state)]() mutable { return std::move(state); });
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
        uint64_t outAddr     = 0;
        uint32_t width       = 0;
        uint32_t height      = 0;
        float    roughness   = 0.0f;
        uint32_t face        = 0;
        uint32_t sampleCount = 0;
    };
};

} // namespace ZHLN::Vk
