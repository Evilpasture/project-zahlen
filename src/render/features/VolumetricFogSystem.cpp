// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "features/VolumetricFogSystem.hpp"
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <vector>

namespace ZHLN {

namespace {

[[nodiscard]] auto Generate3DNoiseData(uint32_t size) -> std::vector<uint8_t> {
    const size_t count = static_cast<size_t>(size) * size * size;
    std::vector<uint8_t> pixels(count * 4);
    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float n = Math::TileableFbm3(
                    {static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, static_cast<float>(z) + 0.5F}, static_cast<float>(size)
                );
                const auto   v   = static_cast<uint8_t>(std::clamp(n * 255.0F, 0.0F, 255.0F));
                const size_t idx = static_cast<size_t>((z * size + y) * size + x) * 4;
                pixels[idx + 0]  = v;
                pixels[idx + 1]  = v;
                pixels[idx + 2]  = v;
                pixels[idx + 3]  = 255;
            }
        }
    }
    return pixels;
}

} // namespace

auto VolumetricFogSystem::Build(RenderContext::Impl& impl) -> std::expected<void, ErrorCode> {
    const auto build = [&impl](auto& pass, const ZHLN_ShaderDesc& shader) -> std::expected<void, ErrorCode> {
        if (auto built = pass.BuildHeap(
                impl.ctx.Device(), impl.heapManager, shader, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, impl.pipelineCache.Get()
            );
            !built) {
            return std::unexpected(built.error());
        }
        return {};
    };

    return build(_clear, Vk::CreateShaderDesc<Shaders::Modules::VolumetricClearCS>())
        .and_then([&]() -> std::expected<void, ErrorCode> { return build(_fogInject, Vk::CreateShaderDesc<Shaders::Modules::VolumetricFogInjectCS>()); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return build(_lightInject, Vk::CreateShaderDesc<Shaders::Modules::VolumetricLightInjectCS>()); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return build(_integrate, Vk::CreateShaderDesc<Shaders::Modules::VolumetricIntegrationCS>()); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return build(_temporal, Vk::CreateShaderDesc<Shaders::Modules::VolumetricTemporalCS>()); });
}

auto VolumetricFogSystem::InitializeNoise(RenderContext::Impl& impl) -> std::expected<void, ErrorCode> {
    constexpr uint32_t kVolumetricNoiseSize = 64;

    const std::vector<uint8_t> pixels = Generate3DNoiseData(kVolumetricNoiseSize);

    return Vk::TextureUploader(impl.ctx, impl.allocator, impl.stagingRingBuffer, impl.graphicsCmdRing)
        .Upload3D(
            {.data = pixels.data(), .width = kVolumetricNoiseSize, .height = kVolumetricNoiseSize, .depth = kVolumetricNoiseSize,
             .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "Volumetric.Noise3D"}
        )
        .transform([this](Vk::TextureResource tex) -> void {
            _noiseImage    = std::move(tex.image);
            _noiseView     = std::move(tex.view);
            _noiseViewInfo = tex.viewInfo;
        });
}

void VolumetricFogSystem::InitSamplers(RenderContext::Impl& impl) noexcept {
    Vk::InitHeapPassSamplers<Shaders::VolumetricTemporal>(
        impl.heapManager, _temporal.heapBindings, Vk::SamplerSlot<"linearSampler">(impl.defaultSamplerInfo)
    );

    // The noise volume tiles, so it repeats; the cascade map is sampled the
    // same way the raster shadow lookup does it, with the comparison sampler.
    const VkSamplerCreateInfo repeatInfo = Vk::SamplerBuilder {}.Linear().Repeat().LodRange(0.0F, 0.0F).Info();
    Vk::InitHeapPassSamplers<Shaders::VolumetricFogInject>(
        impl.heapManager, _fogInject.heapBindings, Vk::SamplerSlot<"noiseSampler">(repeatInfo)
    );
    Vk::InitHeapPassSamplers<Shaders::VolumetricLightInject>(
        impl.heapManager, _lightInject.heapBindings, Vk::SamplerSlot<"shadowSampler">(impl.shadowSamplerInfo)
    );
}

auto VolumetricFogSystem::Clear() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricClearLayout>& {
    return _clear;
}

auto VolumetricFogSystem::FogInject() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout>& {
    return _fogInject;
}

auto VolumetricFogSystem::LightInject() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout>& {
    return _lightInject;
}

auto VolumetricFogSystem::Integrate() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout>& {
    return _integrate;
}

auto VolumetricFogSystem::Temporal() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout>& {
    return _temporal;
}

auto VolumetricFogSystem::NoiseWrite() const noexcept -> Vk::ImageWrite {
    return Vk::ImageWrite {.view = _noiseView.Get(), .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .viewInfo = &_noiseViewInfo};
}

auto VolumetricFogSystem::VoxelDispatchExtent() const noexcept -> VkExtent3D {
    const auto& dispatch = _clear.fixedDispatchSize;
    return {.width = dispatch[0], .height = dispatch[1], .depth = dispatch[2]};
}

}
