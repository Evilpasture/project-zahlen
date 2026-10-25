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
            globalSampler = Vk::Sampler::LinearRepeat()
                                .WithAnisotropy(ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy)
                                .WithLodRange(0.0F, VK_LOD_CLAMP_NONE);
            clampSampler  = Vk::Sampler::LinearClampToEdge();

            return InitSceneHeaps(globalSampler, clampSampler);
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

auto RenderContext::Impl::InitSceneHeaps(Vk::Sampler globalSamplerValue, Vk::Sampler clampSamplerValue) noexcept
    -> std::expected<void, ErrorCode> {
    globalSampler = globalSamplerValue;
    clampSampler  = clampSamplerValue;
    auto init_res = heapManager.Init(
        ctx, allocator, kSceneStaticResourceSlots + kGlobalTextureSlots, kSceneStaticSamplerSlots + kPassStaticSamplerSlots, kFrameTransientResourceSlots,
        kImmediateTransientResourceSlots
    );
    if (!init_res) {
        return std::unexpected(init_res.error());
    }

    if (heapManager.PushDataMaxSize() < GpuAbi::kScenePushLayout.requiredSize) [[unlikely]] {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    // Registration is one step: a handle that comes back is already written.
    auto globalSlot = heapManager.RegisterSampler(globalSampler);
    auto clampSlot  = heapManager.RegisterSampler(clampSampler);
    // The point sampler is chosen by the post-process feature after this point, so
    // its handle is reserved and bound there.
    auto pointSlot = heapManager.ReserveSampler();
    if (!globalSlot || !clampSlot || !pointSlot) {
        return std::unexpected(Vk::DescriptorHeapError::SamplerSlotsExhausted);
    }
    globalSamplerSlot = *globalSlot;
    clampSamplerSlot  = *clampSlot;
    pointSamplerSlot  = *pointSlot;

    constexpr std::array modes = {
        Vk::SamplerAddressMode::Repeat,
        Vk::SamplerAddressMode::ClampToEdge,
        Vk::SamplerAddressMode::MirroredRepeat,
    };
    static_assert(kMaterialSamplerVariantCount == modes.size() * modes.size());
    auto materialBase = heapManager.RegisterSamplerArray(kMaterialSamplerVariantCount, [&](uint32_t variant) {
        const uint32_t s = variant / static_cast<uint32_t>(modes.size());
        const uint32_t t = variant % static_cast<uint32_t>(modes.size());
        return globalSampler.WithLodRange(0.0F, VK_LOD_CLAMP_NONE).WithAddressModes(modes[s], modes[t], modes[0]);
    });
    if (!materialBase) {
        return std::unexpected(materialBase.error());
    }
    materialSamplerBaseSlot = *materialBase;

    // The IBL and lighting images do not exist yet: their mappings are baked into
    // pipelines first, so the handles are reserved here and bound when the images
    // are created.
    auto iblSlot   = heapManager.ReserveImage();
    auto brdfSlot  = heapManager.ReserveImage();
    auto transSlot = heapManager.ReserveImage();
    auto depthSlot = heapManager.ReserveImage();
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

    BuildSceneHeapMappings();

    return {};
}

void RenderContext::Impl::BuildSceneHeapMappings() noexcept {
    // One GlobalSceneRegistry layout, two descriptor sets: the scene block is
    // addressable at Set 0 (its own heap passes) and Set 1 (shared into other
    // passes' pMappings at pipeline build). Both mappings are complete;
    // extras for undeclared variables are legal, missing ones are not
    // (VUID-VkGraphicsPipelineCreateInfo-flags-11312).
    const auto buildSceneBlock = [this](uint32_t set) {
        auto builder = Vk::HeapMappingBuilder(heapManager);
        std::move(builder).Sampler(set, 0, globalSamplerSlot);
        // The six frame addresses: binding numbers and push offsets come from
        // the generated metadata (the Slang field <-> binding association),
        // not a positional six-element convention.
        for (const auto& entry : Slang::kFrameAddressMeta) {
            if (entry.uniformBuffer) {
                std::move(builder).UniformBufferAddress(set, entry.binding, entry.offset);
            } else {
                std::move(builder).StorageBufferAddress(set, entry.binding, entry.offset);
            }
        }
        return std::move(builder)
            .SampledImage(set, 7, iblPrefilteredSlot)
            .SampledImage(set, 8, iblBrdfLutSlot)
            .Sampler(set, 9, clampSamplerSlot)
            .SampledImage(set, 10, transLightingSlot)
            .SamplerArray(set, 11, materialSamplerBaseSlot)
            .BindlessTextureArray(set, 12, textureManager.BindlessBaseHandle())
            .Build();
    };

    sceneHeapMappings     = buildSceneBlock(0);
    sceneSet1HeapMappings = buildSceneBlock(1);
}

void RenderContext::Impl::BuildDecalHeapMappings() noexcept {
    BuildSceneHeapMappings();

    decalHeapMappings = Vk::HeapMappingBuilder(heapManager).SampledImage(0, 0, decalDepthSlot).Sampler(0, 1, pointSamplerSlot).Build();
}

void RenderContext::Impl::WriteSceneStaticImageDescriptors() noexcept {
    if (bindlessLayout.HasBinding(0, 7) && iblPayload.prefilteredView.Handle() != VK_NULL_HANDLE) {
        heapManager.BindImage(iblPrefilteredSlot, iblPayload.prefilteredView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (bindlessLayout.HasBinding(0, 8) && iblPayload.brdfLutView.Handle() != VK_NULL_HANDLE) {
        heapManager.BindImage(iblBrdfLutSlot, iblPayload.brdfLutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void RenderContext::Impl::WritePointSamplerToHeap(Vk::Sampler pointSamplerValue) noexcept {
    heapManager.BindSampler(pointSamplerSlot, pointSamplerValue);
}

void RenderContext::Impl::WriteTransLightingToHeap() noexcept {
    if (!graphResources.transLightingTarget.Valid() || !transLightingSlot.Valid()) {
        return;
    }

    heapManager.BindImage(transLightingSlot, graphResources.transLightingTarget.image.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void RenderContext::Impl::InitPassSamplerDescriptors() noexcept {
    // An equirectangular lookup wraps around the horizon and clamps at the poles.
    const Vk::Sampler skySampler = Vk::Sampler::LinearRepeat().WithAddressModes(
        Vk::SamplerAddressMode::Repeat, Vk::SamplerAddressMode::ClampToEdge, Vk::SamplerAddressMode::ClampToEdge
    );

    Vk::InitHeapPassSamplers<Shaders::Hiz>(heapManager, culling.hizHeapBindings, Vk::UnreadSampler<"pointSampler">(pointSampler));
    Vk::InitHeapPassSamplers<Shaders::Culling>(heapManager, culling.cullingHeapBindings, Vk::SamplerSlot<"g_pointSampler">(pointSampler));
    Vk::InitHeapPassSamplers<Shaders::Lighting>(
        heapManager, post.lighting.heapBindings, Vk::SamplerSlot<"smp">(defaultSampler), Vk::SamplerSlot<"shadowSampler">(shadowSampler),
        Vk::SamplerSlot<"clampSampler">(clampSampler), Vk::SamplerSlot<"pointSampler">(pointSampler)
    );
    Vk::InitHeapPassSamplers<Shaders::Reflection>(
        heapManager, reflectionPipeline.pass.heapBindings, Vk::SamplerSlot<"smp">(defaultSampler), Vk::SamplerSlot<"pointSampler">(pointSampler),
        Vk::SamplerSlot<"clampSampler">(clampSampler), Vk::SamplerSlot<"skySampler">(skySampler)
    );
    Vk::InitHeapPassSamplers<Shaders::Taa>(heapManager, antiAliasing.taa.heapBindings, Vk::SamplerSlot<"smp">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::Fxaa>(heapManager, antiAliasing.fxaa.heapBindings, Vk::SamplerSlot<"smp">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::Mlaa>(heapManager, antiAliasing.mlaa.heapBindings, Vk::SamplerSlot<"sPoint">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::SmaaEdge>(heapManager, antiAliasing.smaaEdge.heapBindings, Vk::SamplerSlot<"pointSampler">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::SmaaWeight>(heapManager, antiAliasing.smaaWeight.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::SmaaBlend>(heapManager, antiAliasing.smaaBlend.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::Blit>(heapManager, post.blit.heapBindings, Vk::SamplerSlot<"smp">(defaultSampler));

    // Lighting and post-process samplers are registered here with every other
    // pass's, reading each bundle's bindings directly.
    Vk::InitHeapPassSamplers<Shaders::Gtao>(heapManager, lighting.gtao.Bindings(), Vk::SamplerSlot<"pointSampler">(pointSampler));
    Vk::InitHeapPassSamplers<Shaders::BloomThreshold>(heapManager, post.bloomThreshold.Bindings(), Vk::SamplerSlot<"smp">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::BloomDown>(heapManager, post.bloomDown.Bindings(), Vk::SamplerSlot<"smp">(defaultSampler));
    Vk::InitHeapPassSamplers<Shaders::BloomUp>(heapManager, post.bloomUp.Bindings(), Vk::SamplerSlot<"smp">(defaultSampler));
    if (ctx.RayTracingSupported()) {
        Vk::InitHeapPassSamplers<Shaders::RcgiCascadeTraceMerge>(
            heapManager, lighting.rcgiCascadeTraceMerge.Bindings(), Vk::SamplerSlot<"clampSampler">(clampSampler)
        );
        Vk::InitHeapPassSamplers<Shaders::DdgiTrace>(heapManager, lighting.ddgiTrace.Bindings(), Vk::SamplerSlot<"clampSampler">(clampSampler));
    }
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

    const Vk::ImageView matView = matImg->View();
    const Vk::ImageView ampView = ampImg->View();

    ltcMatImage = std::move(*matImg);
    ltcAmpImage = std::move(*ampImg);
    post.ltcMat = matView;
    post.ltcAmp = ampView;
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
    const Vk::Sampler equirectSampler = Vk::Sampler::LinearRepeat().WithAddressModes(
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
    if (impl->iblPayload.contentHash == hash && (mode != 3 || impl->iblPayload.visualSkyView.Handle() != VK_NULL_HANDLE)) {
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
