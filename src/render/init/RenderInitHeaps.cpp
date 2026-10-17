// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../IBLProcessor.hpp"
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstring>
#include <format>

namespace ZHLN {

namespace {

[[nodiscard]] auto HashEnvironmentPixels(const EnvironmentRadianceDesc& desc) noexcept -> uint64_t {
    const uint32_t width  = desc.extent.width;
    const uint32_t height = desc.extent.height;
    uint64_t       hash   = Hash64(reinterpret_cast<const char*>(&width), sizeof(width));
    hash ^= Hash64(reinterpret_cast<const char*>(&height), sizeof(height)) + kGolden64 + (hash << 6) + (hash >> 2);
    hash ^= Hash64(reinterpret_cast<const char*>(desc.rgba.data()), desc.rgba.size_bytes()) + kGolden64 + (hash << 6) + (hash >> 2);
    if (!desc.lightingRgba.empty()) {
        hash ^= Hash64(reinterpret_cast<const char*>(desc.lightingRgba.data()), desc.lightingRgba.size_bytes()) + kGolden64 + (hash << 6) + (hash >> 2);
    }
    if (!desc.diffuseSH.empty()) {
        hash ^= Hash64(reinterpret_cast<const char*>(desc.diffuseSH.data()), desc.diffuseSH.size_bytes()) + kGolden64 + (hash << 6) + (hash >> 2);
    }
    return hash == 0 ? 1 : hash;
}

} // namespace

enum class BindlessSetupError : uint8_t {
    DefaultTextureRegistrationFailed ZHLN_ANNOTATION(ZHLN::Description<"Default bindless texture registration returned unexpected indices"> {}) = 1,
};

auto RenderContext::Impl::InitBindless() -> std::expected<void, ErrorCode> {
    return LoadAndCreateShaders(Vk::MakeStageSource<Shaders::Modules::BasicVS>(), Vk::MakeStageSource<Shaders::Modules::BasicPS>())
        .and_then([&](auto&& basicStages) -> std::expected<void, ErrorCode> {
            const auto basicView     = basicStages.View();
            const auto reflectInputs = std::to_array<Vk::ReflectedStageInput>({
                {.shader = basicView.Vertex(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = basicView.Fragment(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::ShadowAtlasVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::ForwardPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::ParticleUpdateCS>(), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::MeshParticleUpdateCS>(), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
            });
            if (!bindlessLayout.Build(ctx.Device(), std::span {reflectInputs})) {
                return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
            }

            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            // Mip chains are generated on upload (Upload2DDesc::generateMips),
            // and the LOD range must let the sampler use them: clamping to
            // LOD 0 forced every minified texel through mip 0 at full
            // anisotropy -- the worst-case fetch pattern for the gbuffer, and
            // shimmer at distance instead of filtered textures. The Blender
            // Cycles reference renders the fidelity goldens come from mip-map
            // properly, so the clamp was a deviation from them as well.
            globalSamplerConfig = Vk::SamplerConfig::LinearRepeat()
                                      .WithAnisotropy(ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy)
                                      .WithLodRange(0.0F, VK_LOD_CLAMP_NONE);
            clampSamplerConfig = Vk::SamplerConfig::LinearClampToEdge();

            return InitSceneHeaps(globalSamplerConfig, clampSamplerConfig);
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitBakeHeapBindings(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitSkeletalAnimationResources(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitLightingLUTs(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitializeSystemTextures(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitializeBlueNoiseTexture(); })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            WriteSceneStaticImageDescriptors();
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            ZHLN::Log("[RenderInit] Pre-allocating persistently mapped per-frame debug VBOs...");
            size_t bufferSize = kMaxDebugVertices * (sizeof(VertexPosition) + sizeof(VertexSurface));
            for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
                auto gpu_buf_res =
                    Vk::Buffer::Create(allocator, bufferSize, Vk::BufferUsage::Vertex | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU);
                if (!gpu_buf_res) {
                    return std::unexpected(ErrorCode(gpu_buf_res.error()));
                }
                auto gpu_buf = std::move(*gpu_buf_res);

                auto address               = ctx.BufferAddress(gpu_buf.Handle());
                frames.debugMeshHandles[i] = geometry.Adopt(std::move(gpu_buf), kMaxDebugVertices, Vk::DeviceAddress<> {address});
            }
            return {};
        });
}

auto RenderContext::Impl::InitSceneHeaps(const Vk::SamplerConfig& globalSamplerValue, const Vk::SamplerConfig& clampSamplerValue) noexcept
    -> std::expected<void, ErrorCode> {
    globalSamplerConfig = globalSamplerValue;
    clampSamplerConfig  = clampSamplerValue;
    auto init_res       = heapManager.Init(
        ctx, allocator, kSceneStaticResourceSlots + kGlobalTextureSlots, kSceneStaticSamplerSlots + kPassStaticSamplerSlots, kFrameTransientResourceSlots,
        kImmediateTransientResourceSlots
    );
    if (!init_res) {
        return std::unexpected(init_res.error());
    }

    if (heapManager.PushDataMaxSize() < GpuAbi::kScenePushLayout.requiredSize) [[unlikely]] {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto globalSlot = heapManager.AllocateStaticSampler();
    auto clampSlot  = heapManager.AllocateStaticSampler();
    auto pointSlot  = heapManager.AllocateStaticSampler();
    if (!globalSlot || !clampSlot || !pointSlot) {
        return std::unexpected(Vk::DescriptorHeapError::SamplerSlotsExhausted);
    }
    globalSamplerSlot = *globalSlot;
    clampSamplerSlot  = *clampSlot;
    pointSamplerSlot  = *pointSlot;
    auto materialBase = heapManager.ReserveOffsetAddressedSamplerRegion(kMaterialSamplerVariantCount);
    if (!materialBase) {
        return std::unexpected(materialBase.error());
    }
    materialSamplerBaseSlot = {*materialBase};

    auto iblSlot   = heapManager.AllocateStaticResource<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>();
    auto brdfSlot  = heapManager.AllocateStaticResource<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>();
    auto transSlot = heapManager.AllocateStaticResource<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>();
    auto depthSlot = heapManager.AllocateStaticResource<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>();
    if (!iblSlot || !brdfSlot || !transSlot || !depthSlot) {
        return std::unexpected(Vk::DescriptorHeapError::ResourceSlotsExhausted);
    }
    iblPrefilteredSlot = *iblSlot;
    iblBrdfLutSlot     = *brdfSlot;
    transLightingSlot  = *transSlot;
    decalDepthSlot     = *depthSlot;

    if (auto reserved = textureManager.ReserveBindlessRegion(); !reserved) [[unlikely]] {
        return std::unexpected(reserved.error());
    }

    heapManager.WriteSampler(globalSamplerSlot, globalSamplerConfig);
    heapManager.WriteSampler(clampSamplerSlot, clampSamplerConfig);

    constexpr std::array modes = {
        Vk::SamplerAddressMode::Repeat,
        Vk::SamplerAddressMode::ClampToEdge,
        Vk::SamplerAddressMode::MirroredRepeat,
    };
    static_assert(kMaterialSamplerVariantCount == modes.size() * modes.size());
    for (uint32_t s = 0; s < modes.size(); ++s) {
        for (uint32_t t = 0; t < modes.size(); ++t) {
            const auto config = globalSamplerConfig.WithLodRange(0.0F, VK_LOD_CLAMP_NONE).WithAddressModes(modes[s], modes[t], modes[0]);
            heapManager.WriteSampler({*materialBase + s * 3u + t}, config);
        }
    }

    BuildSceneHeapMappings();

    return {};
}

void RenderContext::Impl::BuildSceneHeapMappings() noexcept {
    sceneHeapMappings = Vk::HeapMappingBuilder(heapManager)
                            .Sampler(0, 0, globalSamplerSlot)
                            .UniformBufferAddress(0, 1, GpuAbi::kScenePushLayout.frameAddressOffsets[0])
                            .StorageBufferAddress(0, 2, GpuAbi::kScenePushLayout.frameAddressOffsets[1])
                            .StorageBufferAddress(0, 3, GpuAbi::kScenePushLayout.frameAddressOffsets[2])
                            .StorageBufferAddress(0, 4, GpuAbi::kScenePushLayout.frameAddressOffsets[3])
                            .StorageBufferAddress(0, 5, GpuAbi::kScenePushLayout.frameAddressOffsets[4])
                            .StorageBufferAddress(0, 6, GpuAbi::kScenePushLayout.frameAddressOffsets[5])
                            .SampledImage(0, 7, iblPrefilteredSlot)
                            .SampledImage(0, 8, iblBrdfLutSlot)
                            .Sampler(0, 9, clampSamplerSlot)
                            .SampledImage(0, 10, transLightingSlot)
                            .SamplerArray(0, 11, materialSamplerBaseSlot)
                            .BindlessTextureArray(0, 12, textureManager.BindlessBaseSlot())
                            .Build();

    // The scene block's Set-1 mapping, complete: any heap pass whose module
    // references `scene` while its own set is Set 0 merges this bundle at
    // pipeline build (the decal pass since day one, the reflection pass since
    // its sky fill reads scene.lights & co). Layout is GlobalSceneRegistry.
    sceneSet1HeapMappings = Vk::HeapMappingBuilder(heapManager)
                                .Sampler(1, 0, globalSamplerSlot)
                                .UniformBufferAddress(1, 1, GpuAbi::kScenePushLayout.frameAddressOffsets[0])
                                .StorageBufferAddress(1, 2, GpuAbi::kScenePushLayout.frameAddressOffsets[1])
                                .StorageBufferAddress(1, 3, GpuAbi::kScenePushLayout.frameAddressOffsets[2])
                                .StorageBufferAddress(1, 4, GpuAbi::kScenePushLayout.frameAddressOffsets[3])
                                .StorageBufferAddress(1, 5, GpuAbi::kScenePushLayout.frameAddressOffsets[4])
                                .StorageBufferAddress(1, 6, GpuAbi::kScenePushLayout.frameAddressOffsets[5])
                                .SampledImage(1, 7, iblPrefilteredSlot)
                                .SampledImage(1, 8, iblBrdfLutSlot)
                                .Sampler(1, 9, clampSamplerSlot)
                                .SampledImage(1, 10, transLightingSlot)
                                .SamplerArray(1, 11, materialSamplerBaseSlot)
                                .BindlessTextureArray(1, 12, textureManager.BindlessBaseSlot())
                                .Build();
}

void RenderContext::Impl::BuildDecalHeapMappings() noexcept {
    BuildSceneHeapMappings();

    decalHeapMappings = Vk::HeapMappingBuilder(heapManager).SampledImage(0, 0, decalDepthSlot).Sampler(0, 1, pointSamplerSlot).Build();
}

void RenderContext::Impl::WriteSceneStaticImageDescriptors() noexcept {
    if (bindlessLayout.HasBinding(0, 7) && iblPayload.prefilteredInfo.image != VK_NULL_HANDLE) {
        heapManager.WriteImage(iblPrefilteredSlot, iblPayload.prefilteredInfo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (bindlessLayout.HasBinding(0, 8) && iblPayload.brdfLutInfo.image != VK_NULL_HANDLE) {
        heapManager.WriteImage(iblBrdfLutSlot, iblPayload.brdfLutInfo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void RenderContext::Impl::WritePointSamplerToHeap(const Vk::SamplerConfig& config) noexcept {
    heapManager.WriteSampler(pointSamplerSlot, config);
}

void RenderContext::Impl::WriteTransLightingToHeap() noexcept {
    if (!graphResources.transLightingTarget.Valid() || !transLightingSlot.Valid()) {
        return;
    }

    heapManager.WriteImage(transLightingSlot, graphResources.transLightingTarget.image.ViewInfo(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void RenderContext::Impl::InitPassSamplerDescriptors() noexcept {
    const Vk::SamplerConfig defaultConfig = defaultSamplerConfig;
    const Vk::SamplerConfig pointConfig   = pointSamplerConfig;
    const Vk::SamplerConfig shadowConfig  = shadowSamplerConfig;
    const Vk::SamplerConfig clampConfig   = clampSamplerConfig;
    const Vk::SamplerConfig skyConfig     = Vk::SamplerConfig::LinearRepeat().WithAddressModes(
        Vk::SamplerAddressMode::Repeat, Vk::SamplerAddressMode::ClampToEdge, Vk::SamplerAddressMode::ClampToEdge
    );
    const Vk::SamplerConfig blueNoiseConfig = blueNoiseSamplerConfig;

    Vk::InitHeapPassSamplers<Shaders::Hiz>(heapManager, culling.hizHeapBindings, Vk::UnreadSampler<"pointSampler">(pointConfig));
    Vk::InitHeapPassSamplers<Shaders::Culling>(heapManager, culling.cullingHeapBindings, Vk::SamplerSlot<"g_pointSampler">(pointConfig));
    Vk::InitHeapPassSamplers<Shaders::Lighting>(
        heapManager, post.lighting.heapBindings, Vk::SamplerSlot<"smp">(defaultConfig), Vk::SamplerSlot<"shadowSampler">(shadowConfig),
        Vk::SamplerSlot<"clampSampler">(clampConfig), Vk::SamplerSlot<"pointSampler">(pointConfig)
    );
    Vk::InitHeapPassSamplers<Shaders::Reflection>(
        heapManager, reflectionPipeline.pass.heapBindings, Vk::SamplerSlot<"smp">(defaultConfig), Vk::SamplerSlot<"pointSampler">(pointConfig),
        Vk::SamplerSlot<"clampSampler">(clampConfig), Vk::SamplerSlot<"skySampler">(skyConfig), Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseConfig),
        // The scene block's 3x3 wrap-mode table (InitSceneHeaps writes it into
        // the offset-addressed sampler region); named here because the sky
        // fill samples materials, and the contract wants every declared
        // sampler of the block spelled. Not in this set's samplerSlots, so the
        // write above never touches the global table.
        Vk::SamplerSlot<"scene.materialSamplers">(defaultConfig)
    );
    Vk::InitHeapPassSamplers<Shaders::Taa>(heapManager, antiAliasing.taa.heapBindings, Vk::SamplerSlot<"smp">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::Fxaa>(heapManager, antiAliasing.fxaa.heapBindings, Vk::SamplerSlot<"smp">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::Mlaa>(heapManager, antiAliasing.mlaa.heapBindings, Vk::SamplerSlot<"sPoint">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::SmaaEdge>(heapManager, antiAliasing.smaaEdge.heapBindings, Vk::SamplerSlot<"pointSampler">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::SmaaWeight>(heapManager, antiAliasing.smaaWeight.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::SmaaBlend>(heapManager, antiAliasing.smaaBlend.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::Blit>(heapManager, post.blit.heapBindings, Vk::SamplerSlot<"smp">(defaultConfig));

    postProcess.InitSamplers(*this);
    fog.InitSamplers(*this);
}

auto RenderContext::Impl::InitSkeletalAnimationResources() -> std::expected<void, ErrorCode> {
    defer                  rollback([&] {
        for (auto& buffer: frames.jointBuffers) {
            allocator.DestroyBuffer(buffer);
        }
        allocator.DestroyBuffer(morphDeltasBuffer);
    });
    JPH::Array<JPH::Mat44> identities(8192, JPH::Mat44::sIdentity());
    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
        auto jb_res = Vk::Buffer::Create(
            allocator, sizeof(JPH::Mat44) * 8192, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU
        );
        if (!jb_res) {
            return std::unexpected(ErrorCode(jb_res.error()));
        }
        frames.jointBuffers[i] = std::move(*jb_res);
        Vk::Debug::SetBufferName(ctx, frames.jointBuffers[i].Handle(), std::format("JointPalette.Frame{}", i));

        auto mapped = frames.jointBuffers[i].Map(allocator);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }
        std::memcpy(mapped->Data(), identities.data(), identities.size() * sizeof(JPH::Mat44));
    }

    auto mdb_res =
        Vk::Buffer::Create(allocator, sizeof(float) * 4 * 1000000, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU);
    if (!mdb_res) {
        return std::unexpected(ErrorCode(mdb_res.error()));
    }
    morphDeltasBuffer = std::move(*mdb_res);
    Vk::Debug::SetBufferName(ctx, morphDeltasBuffer.Handle(), "MorphDeltas");
    rollback.Dismiss();
    return {};
}

auto RenderContext::Impl::InitLightingLUTs() -> std::expected<void, ErrorCode> {
    auto started = Vk::StagingContext::Begin(allocator, ctx);
    if (!started) {
        return std::unexpected(started.error());
    }
    auto staging = std::move(*started);

    auto ibl = IBLProcessor::Bake(*this);
    if (!ibl) {
        return std::unexpected(ibl.error());
    }
    iblPayload = std::move(*ibl);
    ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

    using namespace Resource;
    constexpr size_t kDdsHeaderBytes = 128;
    const size_t     matRawSize      = ltc_mat.size() - kDdsHeaderBytes;
    const size_t     ampRawSize      = ltc_amp.size() - kDdsHeaderBytes;
    auto             stagingRes      = Vk::Buffer::Create(allocator, matRawSize + ampRawSize, Vk::BufferUsage::TransferSrc, Vk::MemoryUsage::CPUOnly);
    if (!stagingRes) {
        return std::unexpected(stagingRes.error());
    }
    auto  ltcStaging = std::move(*stagingRes);
    defer _([&] { allocator.DestroyBuffer(ltcStaging); });
    {
        auto mapped = ltcStaging.Map(allocator);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }
        std::memcpy(mapped->Data(), ltc_mat.data() + kDdsHeaderBytes, matRawSize);
        std::memcpy(mapped->As<std::byte>() + matRawSize, ltc_amp.data() + kDdsHeaderBytes, ampRawSize);
    }

    constexpr auto kLtcUsage = Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled;
    auto           makeLtc   = [&] { return Vk::Image::Create(allocator, Vk::ImageConfig::Texture2D({64, 64}, VK_FORMAT_R16G16B16A16_SFLOAT, kLtcUsage)); };
    bool           submitted = false;
    auto           matImg    = makeLtc();
    if (!matImg) {
        return std::unexpected(matImg.error());
    }
    defer _([&] {
        if (submitted) {
            submittedStaging->Wait();
        }
        allocator.DestroyImage(*matImg);
    });
    auto  ampImg = makeLtc();
    if (!ampImg) {
        return std::unexpected(ampImg.error());
    }
    defer _([&] {
        if (submitted) {
            submittedStaging->Wait();
        }
        allocator.DestroyImage(*ampImg);
    });

    defer _([&] { std::move(staging).Abort(); });
    staging.UploadImage2DBuffer(matImg->Handle(), 64, 64, 1, ltcStaging.Handle(), 0);
    staging.UploadImage2DBuffer(ampImg->Handle(), 64, 64, 1, ltcStaging.Handle(), matRawSize);
    staging.AddBuffer(std::move(ltcStaging));
    auto work = std::move(staging).ExecuteAsync();
    if (!work) {
        return std::unexpected(work.error());
    }
    submittedStaging = std::make_unique<Vk::SubmittedStagingWork>(std::move(*work));
    submitted        = true;

    const VkImageViewCreateInfo matInfo = matImg->ViewInfo();
    const VkImageViewCreateInfo ampInfo = ampImg->ViewInfo();

    ltcMatImage = std::move(*matImg);
    ltcAmpImage = std::move(*ampImg);
    post.ltcMat = matInfo;
    post.ltcAmp = ampInfo;
    ApplyImageDebugNames(*this);
    return {};
}

auto RenderContext::Impl::InitBakeHeapBindings() noexcept -> std::expected<void, ErrorCode> {
    const auto shader = Vk::CreateShaderDesc<Shaders::Modules::ProceduralBakeCS>();
    if (!proceduralBakeDescLayout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, proceduralBakeDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Immediate, bakeHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }

    const auto iblShader = Vk::CreateShaderDesc<Shaders::Modules::IblSpecularCS>();
    if (!iblBakeDescLayout.Build(ctx.Device(), iblShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, iblBakeDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Immediate, iblBakeHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }
    const Vk::SamplerConfig equirectSampler = Vk::SamplerConfig::LinearRepeat().WithAddressModes(
        Vk::SamplerAddressMode::Repeat, Vk::SamplerAddressMode::ClampToEdge, Vk::SamplerAddressMode::ClampToEdge
    );
    Vk::InitHeapPassSamplers<Shaders::IblBake>(heapManager, iblBakeHeapBindings, Vk::SamplerSlot<"radianceSampler">(equirectSampler));
    return {};
}

auto RenderContext::SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept -> std::expected<void, ErrorCode> {
    auto* const impl      = _impl.get();
    const bool  hasPixels = !desc.rgba.empty();
    if (hasPixels && (desc.extent.width > kMaxEnvironmentRadianceExtent || desc.extent.height > kMaxEnvironmentRadianceExtent)) {
        return std::unexpected(EnvironmentBakeError::RadianceTooLarge);
    }
    if ((hasPixels &&
         (desc.extent.width == 0 || desc.extent.height == 0 || desc.rgba.size() != static_cast<size_t>(desc.extent.width) * desc.extent.height * 4)) ||
        (!hasPixels && (desc.extent.width != 0 || desc.extent.height != 0)) || (!desc.lightingRgba.empty() && desc.lightingRgba.size() != desc.rgba.size()) ||
        (!desc.diffuseSH.empty() && (desc.lightingRgba.empty() || desc.diffuseSH.size() != 9))) {
        return std::unexpected(EnvironmentBakeError::InvalidRadianceData);
    }

    const uint64_t hash = hasPixels ? desc.contentHash != 0 ? desc.contentHash : HashEnvironmentPixels(desc) : 0;
    const int      mode = hasPixels ? !desc.renderSkybox ? 2 : (desc.lightingRgba.empty() ? 1 : 3) : 0;
    if (impl->iblPayload.contentHash == hash && (mode != 3 || impl->iblPayload.visualSkyInfo.image != VK_NULL_HANDLE)) {
        impl->iblPayload.environmentMode = mode;
        return {};
    }

    const auto& sky = impl->settings.environment;

    const auto source = hasPixels ?
                            IBLProcessor::RadianceSource {
                                .rgba         = desc.lightingRgba.empty() ? desc.rgba.data() : desc.lightingRgba.data(),
                                .visualRgba   = desc.lightingRgba.empty() ? nullptr : desc.rgba.data(),
                                .diffuseSH    = desc.diffuseSH,
                                .width        = desc.extent.width,
                                .height       = desc.extent.height,
                                .renderSkybox = desc.renderSkybox ? 1 : 0
                            } :
                            IBLProcessor::RadianceSource {};

    if (auto waited = Vk::WaitIdle(impl->ctx.Device()); !waited) {
        return std::unexpected(waited.error());
    }
    auto baked = IBLProcessor::Bake(*impl, sky, source);
    if (!baked) {
        return std::unexpected(baked.error());
    }
    baked->contentHash = hash;

    std::swap(impl->iblPayload, *baked);
    impl->WriteSceneStaticImageDescriptors();
    baked->Destroy(impl->allocator);
    return {};
}

auto RenderContext::Impl::InitializeSystemTextures() noexcept -> std::expected<void, ErrorCode> {
    ZHLN::Log("[Resource Factory] Registering fallback system texture slots...");

    std::array<uint8_t, 4> blackPixel  = {0, 0, 0, 0};
    std::array<uint8_t, 4> whitePixel  = {255, 255, 255, 255};
    std::array<uint8_t, 4> normalPixel = {128, 128, 255, 255};

    return textureManager.Upload2D(blackPixel.data(), 1, 1, Rgba8Format(false))
        .and_then([&, whitePixel, normalPixel](uint32_t blackIdx) -> std::expected<void, ErrorCode> {
            return textureManager.Upload2D(whitePixel.data(), 1, 1, Rgba8Format(true))
                .and_then([&, blackIdx, normalPixel](uint32_t whiteIdx) -> std::expected<void, ErrorCode> {
                    return textureManager.Upload2D(normalPixel.data(), 1, 1, Rgba8Format(false))
                        .and_then([&, blackIdx, whiteIdx](uint32_t normalIdx) -> std::expected<void, ErrorCode> {
                            if (blackIdx != kFallbackBlackTextureIndex || whiteIdx != kFallbackWhiteTextureIndex || normalIdx != kFallbackNormalTextureIndex) {
                                return std::unexpected(BindlessSetupError::DefaultTextureRegistrationFailed);
                            }
                            return {};
                        });
                });
        });
}

} // namespace ZHLN
