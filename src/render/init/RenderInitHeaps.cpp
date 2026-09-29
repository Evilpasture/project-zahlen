// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../IBLProcessor.hpp"
#include "../RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include "../Resources.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/RadianceMap.hpp>
#include <array>
#include <cstring>

namespace ZHLN {

enum class BindlessSetupError : uint8_t {
    DefaultTextureRegistrationFailed ZHLN_ANNOTATION(ZHLN::Description<"Default bindless texture registration returned unexpected indices">{}) = 1,
};

auto RenderContext::Impl::InitBindless() -> std::expected<void, ErrorCode> {
    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BasicVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::BasicPS>()
    )
        .and_then([&](auto&& basicStages) -> std::expected<void, ErrorCode> {
            // The descriptors' entry-point pointers refer to this view's
            // inline names; keep it alive through layout reflection.
            const auto basicView = basicStages.View();
            const Vk::ReflectedStageInput reflectInputs[6] = {
                {.shader = basicView.Vertex(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = basicView.Fragment(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::PunctualShadowsVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::ForwardPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::ParticleUpdateCS>(), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::MeshParticleUpdateCS>(), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
            };
            if (!bindlessLayout.Build(ctx.Device(), std::span {reflectInputs})) {
                return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
            }

            emptyPipelineLayout = VK_NULL_HANDLE;
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto globalBuilder =
                Vk::SamplerBuilder {}.Linear().Repeat().Anisotropy(ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy).LodRange(0.0f, 0.0f);
            auto clampBuilder = Vk::SamplerBuilder {}.Linear().ClampToEdge();

            return globalBuilder.Build(ctx.Device())
                .transform_error([](auto err) -> ErrorCode { return err; })
                .and_then([&](auto&& globalRes) -> std::expected<void, ErrorCode> {
                    globalSampler = std::forward<decltype(globalRes)>(globalRes);
                    return clampBuilder.Build(ctx.Device())
                        .transform_error([](auto err) -> ErrorCode { return err; })
                        .and_then([&](auto&& clampRes) -> std::expected<void, ErrorCode> {
                            clampSampler = std::forward<decltype(clampRes)>(clampRes);
                            return InitSceneHeaps(globalBuilder.Info(), clampBuilder.Info());
                        });
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitBakeHeapBindings(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitSkeletalAnimationResources(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitLightingLUTs(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return InitializeSystemTextures(); })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return InitializeBlueNoiseTexture();
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            WriteSceneStaticImageDescriptors();
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            ZHLN::Log("[RenderInit] Pre-allocating persistently mapped per-frame debug VBOs...");
            size_t bufferSize = kMaxDebugVertices * (sizeof(VertexPosition) + sizeof(VertexAttributes));
            for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
                auto gpu_buf_res = Vk::Buffer::Create(
                    allocator.Get(), bufferSize, Vk::BufferUsage::Vertex | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU
                );
                if (!gpu_buf_res) {
                    return std::unexpected(ErrorCode(gpu_buf_res.error()));
                }
                auto gpu_buf = std::move(*gpu_buf_res);

                auto address               = ctx.BufferAddress(gpu_buf.Handle());
                frames.debugMeshHandles[i] = geometry.Adopt(std::move(gpu_buf), kMaxDebugVertices, address);
            }
            return {};
        });
}

auto RenderContext::Impl::InitSceneHeaps(const VkSamplerCreateInfo& globalSamplerInfo, const VkSamplerCreateInfo& clampSamplerInfo) noexcept
    -> std::expected<void, ErrorCode> {

    auto init_res = heapManager.Init(
        ctx, allocator, kSceneStaticResourceSlots + kGlobalTextureSlots, kSceneStaticSamplerSlots + kPassStaticSamplerSlots,
        kFrameTransientResourceSlots, kImmediateTransientResourceSlots
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
    materialSamplerBaseSlot = Vk::SamplerHandle {*materialBase};

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

    heapManager.WriteSampler(globalSamplerSlot, globalSamplerInfo);
    heapManager.WriteSampler(clampSamplerSlot, clampSamplerInfo);

    constexpr std::array<VkSamplerAddressMode, 3> modes = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
    };
    static_assert(kMaterialSamplerVariantCount == modes.size() * modes.size());
    for (uint32_t s = 0; s < modes.size(); ++s) {
        for (uint32_t t = 0; t < modes.size(); ++t) {
            auto info = globalSamplerInfo; // Keep filtering/aniso; let glTF materials use generated mips.
            info.maxLod = VK_LOD_CLAMP_NONE;
            info.addressModeU = modes[s];
            info.addressModeV = modes[t];
            heapManager.WriteSampler(Vk::SamplerHandle {*materialBase + s * 3u + t}, info);
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

    decalSceneHeapMappings = Vk::HeapMappingBuilder(heapManager)
        .Sampler(1, 0, globalSamplerSlot)
        .UniformBufferAddress(1, 1, GpuAbi::kScenePushLayout.frameAddressOffsets[0])
        .SamplerArray(1, 11, materialSamplerBaseSlot)
        .BindlessTextureArray(1, 12, textureManager.BindlessBaseSlot())
        .Build();
}

void RenderContext::Impl::BuildDecalHeapMappings() noexcept {
    BuildSceneHeapMappings();

    decalHeapMappings = Vk::HeapMappingBuilder(heapManager)
        .SampledImage(0, 0, decalDepthSlot)
        .Sampler(0, 1, pointSamplerSlot)
        .Build();
}

void RenderContext::Impl::WriteSceneStaticImageDescriptors() noexcept {
    if (bindlessLayout.HasBinding(0, 7) && iblPayload.prefilteredView.Valid()) {
        heapManager.WriteImage(iblPrefilteredSlot, iblPayload.prefilteredView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (bindlessLayout.HasBinding(0, 8) && iblPayload.brdfLutView.Valid()) {
        heapManager.WriteImage(iblBrdfLutSlot, iblPayload.brdfLutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void RenderContext::Impl::WritePointSamplerToHeap(const VkSamplerCreateInfo& info) noexcept {
    heapManager.WriteSampler(pointSamplerSlot, info);
}

void RenderContext::Impl::WriteTransLightingToHeap() noexcept {
    if (!graphResources.transLightingTarget.Valid() || !transLightingSlot.Valid()) {
        return;
    }
    heapManager.WriteImage(transLightingSlot, graphResources.transLightingTarget.fullView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void RenderContext::Impl::InitPassSamplerDescriptors() noexcept {
    const VkSamplerCreateInfo defaultInfo = defaultSamplerInfo;
    const VkSamplerCreateInfo pointInfo   = pointSamplerInfo;
    const VkSamplerCreateInfo shadowInfo  = shadowSamplerInfo;
    const VkSamplerCreateInfo clampInfo   = [&]() -> VkSamplerCreateInfo {
        return Vk::SamplerBuilder {}.Linear().ClampToEdge().Info();
    }();

    Vk::InitHeapPassSamplers<Shaders::Hiz>(heapManager, hizHeapBindings, Vk::UnreadSampler<"pointSampler">(pointInfo));
    Vk::InitHeapPassSamplers<Shaders::Culling>(heapManager, cullingHeapBindings, Vk::SamplerSlot<"g_pointSampler">(pointInfo));
    const VkSamplerCreateInfo blueNoiseInfo = Vk::SamplerBuilder {}.Nearest().Repeat().LodRange(0.0F, 0.0F).Info();
    Vk::InitHeapPassSamplers<Shaders::Lighting>(
        heapManager, lightingPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo), Vk::SamplerSlot<"shadowSampler">(shadowInfo),
        Vk::SamplerSlot<"clampSampler">(clampInfo), Vk::SamplerSlot<"pointSampler">(pointInfo), Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseInfo)
    );
    Vk::InitHeapPassSamplers<Shaders::Reflection>(
        heapManager, reflectionPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo), Vk::SamplerSlot<"pointSampler">(pointInfo),
        Vk::SamplerSlot<"clampSampler">(clampInfo), Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseInfo)
    );
    Vk::InitHeapPassSamplers<Shaders::Reflection>(
        heapManager, translucentReflectionPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo), Vk::SamplerSlot<"pointSampler">(pointInfo),
        Vk::SamplerSlot<"clampSampler">(clampInfo), Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseInfo)
    );
    Vk::InitHeapPassSamplers<Shaders::Taa>(heapManager, taaPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::Fxaa>(heapManager, fxaaPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::Mlaa>(heapManager, mlaaPass.heapBindings, Vk::SamplerSlot<"sPoint">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::SmaaEdge>(heapManager, smaaEdgePass.heapBindings, Vk::SamplerSlot<"pointSampler">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::SmaaWeight>(heapManager, smaaWeightPass.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::SmaaBlend>(heapManager, smaaBlendPass.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::Blit>(heapManager, blitPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    // The passes below own their own pipelines and heap bindings, so they
    // write the sampler descriptors those bindings declare.
    postProcess.InitSamplers(*this);
    fog.InitSamplers(*this);
}

auto RenderContext::Impl::InitSkeletalAnimationResources() -> std::expected<void, ErrorCode> {
    defer rollback([&] {
        for (auto& buffer: frames.jointBuffers) allocator.DestroyBuffer(buffer);
        allocator.DestroyBuffer(morphDeltasBuffer);
    });
    JPH::Array<JPH::Mat44> identities(8192, JPH::Mat44::sIdentity());
    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
        auto jb_res = Vk::Buffer::Create(
            allocator.Get(), sizeof(JPH::Mat44) * 8192, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
            Vk::MemoryUsage::CPUToGPU
        );
        if (!jb_res) {
            return std::unexpected(ErrorCode(jb_res.error()));
        }
        frames.jointBuffers[i] = std::move(*jb_res);

        auto mapped = frames.jointBuffers[i].Map(allocator.Get());
        if (mapped.data == nullptr) return std::unexpected(Vk::StagingError::MemoryMappingFailed);
        std::memcpy(mapped.data, identities.data(), identities.size() * sizeof(JPH::Mat44));
    }

    auto mdb_res = Vk::Buffer::Create(
        allocator.Get(), sizeof(float) * 4 * 1000000, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
        Vk::MemoryUsage::CPUToGPU
    );
    if (!mdb_res) {
        return std::unexpected(ErrorCode(mdb_res.error()));
    }
    morphDeltasBuffer = std::move(*mdb_res);
    rollback.Dismiss();
    return {};
}

auto RenderContext::Impl::InitLightingLUTs() -> std::expected<void, ErrorCode> {
    stagingContext = std::make_unique<Vk::StagingContext>(allocator, ctx);
    if (auto started = stagingContext->Begin(); !started) return std::unexpected(started.error());

    auto ibl = Vk::IBLProcessor::Bake(*this);
    if (!ibl) return std::unexpected(ibl.error());
    iblPayload = std::move(*ibl);
    ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

    using namespace Resource;
    constexpr size_t kDdsHeaderBytes = 128;
    const size_t matRawSize = ltc_mat.size() - kDdsHeaderBytes;
    const size_t ampRawSize = ltc_amp.size() - kDdsHeaderBytes;
    auto stagingRes = Vk::Buffer::Create(allocator.Get(), matRawSize + ampRawSize, Vk::BufferUsage::TransferSrc, Vk::MemoryUsage::CPUOnly);
    if (!stagingRes) return std::unexpected(stagingRes.error());
    auto ltcStaging = std::move(*stagingRes);
    defer _([&] { allocator.DestroyBuffer(ltcStaging); });
    {
        auto mapped = ltcStaging.Map(allocator.Get());
        if (mapped.data == nullptr) return std::unexpected(Vk::StagingError::MemoryMappingFailed);
        std::memcpy(mapped.data, ltc_mat.data() + kDdsHeaderBytes, matRawSize);
        std::memcpy(static_cast<std::byte*>(mapped.data) + matRawSize, ltc_amp.data() + kDdsHeaderBytes, ampRawSize);
    }

    constexpr auto kLtcUsage = Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled;
    auto makeLtc = [&] {
        return Vk::ImageBuilder {}.Texture2D(64, 64, VK_FORMAT_R16G16B16A16_SFLOAT, kLtcUsage, 1).Build(allocator.Get());
    };
    bool submitted = false;
    auto matImg = makeLtc();
    if (!matImg) return std::unexpected(matImg.error());
    defer _([&] {
        if (submitted) stagingContext->Wait();
        allocator.DestroyImage(*matImg);
    });
    auto ampImg = makeLtc();
    if (!ampImg) return std::unexpected(ampImg.error());
    defer _([&] {
        if (submitted) stagingContext->Wait();
        allocator.DestroyImage(*ampImg);
    });

    stagingContext->UploadImage2DBuffer(matImg->Handle(), 64, 64, 1, ltcStaging.Handle(), 0);
    stagingContext->UploadImage2DBuffer(ampImg->Handle(), 64, 64, 1, ltcStaging.Handle(), matRawSize);
    stagingContext->AddBuffer(std::move(ltcStaging));
    if (auto executed = stagingContext->ExecuteAsync(); !executed) return std::unexpected(executed.error());
    submitted = true;

    auto matView = Vk::ImageView::Create<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), matImg->Handle());
    if (!matView) return std::unexpected(matView.error());
    auto ampView = Vk::ImageView::Create<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ampImg->Handle());
    if (!ampView) return std::unexpected(ampView.error());

    ltcMatImage = std::move(*matImg);
    ltcAmpImage = std::move(*ampImg);
    ltcMatView = std::move(*matView);
    ltcAmpView = std::move(*ampView);
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
    VkSamplerCreateInfo equirectInfo = Vk::SamplerBuilder {}.Linear().Info();
    equirectInfo.addressModeV        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    equirectInfo.addressModeW        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Vk::InitHeapPassSamplers<Shaders::IblBake>(heapManager, iblBakeHeapBindings, Vk::SamplerSlot<"radianceSampler">(equirectInfo));
    return {};
}

auto RenderContext::SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept -> std::expected<void, ErrorCode> {
    auto* const impl = _impl.get();
    const bool hasPixels = !desc.rgba.empty();
    if (hasPixels && (desc.extent.width > kMaxRadianceExtent || desc.extent.height > kMaxRadianceExtent)) {
        return std::unexpected(Vk::EnvironmentBakeError::RadianceTooLarge);
    }
    if ((hasPixels && (desc.extent.width == 0 || desc.extent.height == 0 ||
                      desc.rgba.size() != static_cast<size_t>(desc.extent.width) * desc.extent.height * 4)) ||
        (!hasPixels && (desc.extent.width != 0 || desc.extent.height != 0))) {
        return std::unexpected(Vk::EnvironmentBakeError::InvalidRadianceData);
    }

    uint64_t hash = 0;
    int      mode = 0;
    if (hasPixels) {
        hash = desc.contentHash != 0 ? desc.contentHash : HashRadiancePixels(desc.rgba.data(), desc.extent.width, desc.extent.height);
        mode = desc.renderSkybox != 0 ? 1 : 2;
    }
    if (impl->iblPayload.contentHash == hash && impl->iblPayload.environmentMode == mode) {
        return {};
    }
    if (impl->iblPayload.contentHash == hash) {
        impl->iblPayload.environmentMode = mode;
        return {};
    }

    Components::PostProcessSettingsComponent sky {};
    const auto& env = impl->settings.environment;
    sky.skyZenith   = JPH::Vec4(env.skyZenith[0], env.skyZenith[1], env.skyZenith[2], env.skyZenith[3]);
    sky.skyHorizon  = JPH::Vec4(env.skyHorizon[0], env.skyHorizon[1], env.skyHorizon[2], env.skyHorizon[3]);
    sky.skyGround   = JPH::Vec4(env.skyGround[0], env.skyGround[1], env.skyGround[2], env.skyGround[3]);

    Vk::IBLProcessor::RadianceSource source {};
    if (hasPixels) {
        source.rgba         = desc.rgba.data();
        source.width        = desc.extent.width;
        source.height       = desc.extent.height;
        source.renderSkybox = desc.renderSkybox ? 1 : 0;
    }
    // Environment changes are rare; wait rather than retiring the old cube
    // and LUT while previous frames can still read their descriptors/views.
    if (auto waited = Vk::WaitIdle(impl->ctx.Device()); !waited) return std::unexpected(waited.error());
    auto baked = Vk::IBLProcessor::Bake(*impl, sky, source);
    if (!baked) {
        return std::unexpected(baked.error());
    }
    baked->contentHash = hash;
    // Swap keeps each view with its image; old resources are now in `baked`.
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

    return textureManager.Upload2D(blackPixel.data(), 1, 1, Rgba8Format(false)).and_then([&, whitePixel, normalPixel](uint32_t blackIdx) -> std::expected<void, ErrorCode> {
        return textureManager.Upload2D(whitePixel.data(), 1, 1, Rgba8Format(true)).and_then([&, blackIdx, normalPixel](uint32_t whiteIdx) -> std::expected<void, ErrorCode> {
            return textureManager.Upload2D(normalPixel.data(), 1, 1, Rgba8Format(false)).and_then([&, blackIdx, whiteIdx](uint32_t normalIdx) -> std::expected<void, ErrorCode> {
                if (blackIdx != kFallbackBlackTextureIndex || whiteIdx != kFallbackWhiteTextureIndex || normalIdx != kFallbackNormalTextureIndex) {
                    return std::unexpected(BindlessSetupError::DefaultTextureRegistrationFailed);
                }
                return {};
            });
        });
    });
}

}
