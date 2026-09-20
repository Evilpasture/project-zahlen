// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/init/RenderInitHeaps.cpp
#include "../IBLProcessor.hpp"
#include "../RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include "../Resources.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstring>

namespace ZHLN {

// Private bindless/heap setup failure (Tier 1): declared at file scope in this
// translation unit so no header exposes it.
enum class BindlessSetupError : uint8_t {
    DefaultTextureRegistrationFailed ZHLN_ANNOTATION(ZHLN::Description<"Default bindless texture registration returned unexpected indices">{}) = 1,
};

auto RenderContext::Impl::InitBindless() -> std::expected<void, ErrorCode> {
    // Reflect the authoritative GlobalSceneRegistry layout out of the compiled
    // scene shaders. The union across every `scene`-consuming entry point
    // (basic VS/PS, forward PS, punctual-shadow VS) covers exactly the registry
    // members in live use: {0,1,2,3,4,5,6,10,11}. Under the descriptor-heap
    // model the reflection no longer produces descriptor set layouts — it only
    // reports which set-0 bindings exist, and the engine maps them onto the
    // heaps below (see BuildSceneHeapMappings).
    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BasicVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::BasicPS>()
    )
        .and_then([&](auto&& basicStages) -> std::expected<void, ErrorCode> {
            const Vk::ReflectedStageInput reflectInputs[6] = {
                {.shader = Vk::CreateShaderDesc(basicStages.GetVertSpv()), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc(basicStages.GetFragSpv()), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::PunctualShadowsVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::ForwardPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                // Compute consumers widen the stage flags of the members they
                // touch (`scene.frame` for both particle simulations). Without
                // them the union reflection would only carry VS|FS stages and
                // the compute-side mappings would be incomplete.
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::ParticleUpdateCS>(), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
                {.shader = Vk::CreateShaderDesc<Shaders::Modules::MeshParticleUpdateCS>(), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
            };
            if (!bindlessLayout.Build(ctx.Device(), std::span {reflectInputs})) {
                return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
            }

            // Descriptor-heap pipelines are created with VK_NULL_HANDLE as
            // their pipeline layout (VUID-VkGraphicsPipelineCreateInfo-
            // flags-11311); there is no layout object to own. The member
            // exists only as a named alias for the null layout.
            emptyPipelineLayout = VK_NULL_HANDLE;
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            // Build the samplers first: their VkSamplerCreateInfo values are
            // what vkWriteSamplerDescriptorsEXT consumes for the sampler heap.
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
            // IBL images exist after InitLightingLUTs; write their heap
            // descriptors once (they never change after init). The translucent
            // lighting + decal depth descriptors are (re)written whenever the
            // targets are recreated.
            WriteSceneStaticImageDescriptors();
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            ZHLN::Log("[RenderInit] Pre-allocating persistently mapped Double-Buffered Debug VBOs...");
            size_t bufferSize = kMaxDebugVertices * (sizeof(VertexPosition) + sizeof(VertexAttributes));
            for (int i = 0; i < 2; ++i) {
                auto gpu_buf_res = Vk::Buffer::Create(
                    allocator.Get(), bufferSize, Vk::BufferUsage::Vertex | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU
                );
                if (!gpu_buf_res) {
                    return std::unexpected(ErrorCode(gpu_buf_res.error()));
                }
                auto gpu_buf = std::move(*gpu_buf_res);

                auto address               = ctx.BufferAddress(gpu_buf.Handle());
                frames.debugMeshHandles[i] = meshPool.Create(std::move(gpu_buf), kMaxDebugVertices, address);
            }
            return {};
        });
}

auto RenderContext::Impl::InitSceneHeaps(const VkSamplerCreateInfo& globalSamplerInfo, const VkSamplerCreateInfo& clampSamplerInfo) noexcept
    -> std::expected<void, ErrorCode> {
    // The push-data layout is not reflected here any more: GpuAbi.hpp reads the
    // ABI module's own bytes at compile time and refuses to build if
    // DescriptorHeapPushData moves a frame address or the descriptor index, so
    // by the time this runs the layout is a fact (`Vk::kHeapPushDataLayout`)
    // rather than a reflection that can fail.

    // Static resource slots hold the scene registry head and the offset-addressed
    // bindless array; every pass block comes from the transient partitions below.
    auto init_res = heapManager.Init(
        ctx, allocator, kSceneStaticResourceSlots + kGlobalTextureSlots, kSceneStaticSamplerSlots + kPassStaticSamplerSlots,
        kFrameTransientResourceSlots, kImmediateTransientResourceSlots, 2
    );
    if (!init_res) {
        return std::unexpected(init_res.error());
    }

    // Slang is still the layout authority for the frame-address fields and the
    // per-dispatch descriptor index; the check above is what keeps the constant
    // honest. What is left to ask at runtime is whether this device's push-data
    // budget fits the layout at all.
    if (heapManager.PushDataMaxSize() < Vk::kHeapPushDataLayout.requiredSize) [[unlikely]] {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    // --- Static slot allocation (sampler heap) ---
    auto globalSlot = heapManager.AllocateStaticSampler();
    auto clampSlot  = heapManager.AllocateStaticSampler();
    auto pointSlot  = heapManager.AllocateStaticSampler();
    if (!globalSlot || !clampSlot || !pointSlot) {
        return std::unexpected(Vk::DescriptorHeapError::SamplerSlotsExhausted);
    }
    globalSamplerSlot = *globalSlot;
    clampSamplerSlot  = *clampSlot;
    pointSamplerSlot  = *pointSlot;

    // --- Static slot allocation (resource heap) ---
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

    // globalTextures[] is one offset-addressed region: the reservation decides
    // where it lands and hands the base back, which is what the set-0 mapping
    // points at. A static allocation added above moves the array instead of
    // silently overlapping it, so the four scene allocations no longer have to
    // be kept in step with a hand-counted cursor skip.
    const auto textureBase = heapManager.ReserveOffsetAddressedResourceRegion(kGlobalTextureSlots);
    if (!textureBase) [[unlikely]] {
        return std::unexpected(textureBase.error());
    }
    textureHeapBase = *textureBase;

    // --- Write the static sampler descriptors into the sampler heap ---
    heapManager.WriteSampler(globalSamplerSlot, globalSamplerInfo);
    heapManager.WriteSampler(clampSamplerSlot, clampSamplerInfo);
    // pointSamplerSlot is written by WritePointSamplerToHeap once the sampler exists.

    // --- Bake the set/binding -> heap mapping tables for pipeline creation ---
    BuildSceneHeapMappings();

    return {};
}

void RenderContext::Impl::BuildSceneHeapMappings() noexcept {
    // GlobalSceneRegistry (common.slang) member order -> binding numbers:
    //   0 defaultSampler    4 g_joints        8 brdfLUT
    //   1 frame             5 g_prevJoints    9 clampSampler
    //   2 lights            6 g_morphDeltas  10 texTransLighting
    //   3 g_instances       7 prefilteredMap 11 globalTextures[]
    //
    // Static samplers/images resolve through constant offsets into the heaps;
    // the per-frame buffers (1..6) carry device addresses in the push-data
    // blob at kHeapPushDataLayout.frameAddressOffsets. May run more than once
    // (initial bake + decal-pipeline bake): each run rebuilds both tables.
    sceneHeapMappings = HeapMappingBuilder(heapManager)
        .Sampler(0, 0, globalSamplerSlot)
        .UniformBufferAddress(0, 1, Vk::kHeapPushDataLayout.frameAddressOffsets[0])
        .StorageBufferAddress(0, 2, Vk::kHeapPushDataLayout.frameAddressOffsets[1])
        .StorageBufferAddress(0, 3, Vk::kHeapPushDataLayout.frameAddressOffsets[2])
        .StorageBufferAddress(0, 4, Vk::kHeapPushDataLayout.frameAddressOffsets[3])
        .StorageBufferAddress(0, 5, Vk::kHeapPushDataLayout.frameAddressOffsets[4])
        .StorageBufferAddress(0, 6, Vk::kHeapPushDataLayout.frameAddressOffsets[5])
        .SampledImage(0, 7, iblPrefilteredSlot)
        .SampledImage(0, 8, iblBrdfLutSlot)
        .Sampler(0, 9, clampSamplerSlot)
        .SampledImage(0, 10, transLightingSlot)
        .BindlessTextureArray(0, 11, textureHeapBase)
        .Build();

    // decal.slang only touches three registry members (defaultSampler, frame
    // and globalTextures -- see the shader), so its scene subset (set 1) maps
    // exactly those.
    decalSceneHeapMappings = HeapMappingBuilder(heapManager)
        .Sampler(1, 0, globalSamplerSlot)
        .UniformBufferAddress(1, 1, Vk::kHeapPushDataLayout.frameAddressOffsets[0])
        .BindlessTextureArray(1, 11, textureHeapBase)
        .Build();
}

void RenderContext::Impl::BuildDecalHeapMappings() noexcept {
    // The scene tables are baked from constants (no reflection input), so this
    // is a plain rebuild of both -- kept so the decal bake re-bakes everything
    // it touches.
    BuildSceneHeapMappings();

    // decal.slang set 0: {binding 0 = texDepth (sampled image), binding 1 = pointSampler}.
    decalHeapMappings = HeapMappingBuilder(heapManager)
        .SampledImage(0, 0, decalDepthSlot)
        .Sampler(0, 1, pointSamplerSlot)
        .Build();
}

void RenderContext::Impl::WriteSceneStaticImageDescriptors() noexcept {
    if (bindlessLayout.HasBinding(0, 7) && iblPayload.prefilteredView.Valid()) {
        constexpr uint32_t kIblMipLevels = 6; // Mirrors the IBL processor's prefiltered cube chain
        const auto         info          = Vk::MakeViewCreateInfoCube(iblPayload.prefilteredImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, kIblMipLevels);
        heapManager.WriteImage(iblPrefilteredSlot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (bindlessLayout.HasBinding(0, 8) && iblPayload.brdfLutView.Valid()) {
        const auto info = Vk::MakeViewCreateInfo2D(iblPayload.brdfLutImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
        heapManager.WriteImage(iblBrdfLutSlot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void RenderContext::Impl::WritePointSamplerToHeap(const VkSamplerCreateInfo& info) noexcept {
    heapManager.WriteSampler(pointSamplerSlot, info);
}

void RenderContext::Impl::WriteTransLightingToHeap() noexcept {
    if (!graphResources.transLightingTarget.Valid() || !transLightingSlot.Valid()) {
        return;
    }
    const auto info = Vk::MakeViewCreateInfo2D(graphResources.transLightingTarget.image.Handle(), VK_FORMAT_R16G16B16A16_SFLOAT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    heapManager.WriteImage(transLightingSlot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void RenderContext::Impl::InitPassSamplerDescriptors() noexcept {
    // Write the static sampler descriptors of every descriptor-heap pass into
    // their allocated sampler-heap slots (each pass baked its own slot at
    // pipeline-build time). Every argument is a Vk::SamplerSlot<"name"> matched
    // against the name SPIRV-Reflect reported for that sampler, so neither
    // declaration order nor the samplers a configuration drops (Slang removes
    // unreferenced parameters) affects which create info lands where.
    const VkSamplerCreateInfo defaultInfo = defaultSamplerInfo;
    const VkSamplerCreateInfo pointInfo   = pointSamplerInfo;
    const VkSamplerCreateInfo shadowInfo  = shadowSamplerInfo;
    const VkSamplerCreateInfo clampInfo   = [&]() -> VkSamplerCreateInfo {
        // clampSampler is the linear clamp-to-edge sampler; its create info was
        // captured at InitBindless time (kept in the heap slot already) — for
        // pass slots we re-derive it identically.
        return Vk::SamplerBuilder {}.Linear().ClampToEdge().Info();
    }();

    // hiz_generate.slang declares pointSampler without ever sampling with it, so
    // Slang strips the binding and this write is a no-op -- naming it keeps the
    // call correct if a future HiZ pass starts using the sampler.
    Vk::InitHeapPassSamplers<Shaders::Hiz>(heapManager, hizHeapBindings, Vk::UnreadSampler<"pointSampler">(pointInfo));
    Vk::InitHeapPassSamplers<Shaders::Culling>(heapManager, cullingHeapBindings, Vk::SamplerSlot<"g_pointSampler">(pointInfo));
    // ao_gtao.slang declares exactly one sampler, pointSampler (fixed-lod
    // nearest taps for depth, normals and the half-res AO target).
    Vk::InitHeapPassSamplers<Shaders::Gtao>(heapManager, gtaoHeapBindings, Vk::SamplerSlot<"pointSampler">(pointInfo));
    Vk::InitHeapPassSamplers<Shaders::BloomThreshold>(heapManager, bloomThresholdHeapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::BloomDown>(heapManager, bloomDownHeapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::BloomUp>(heapManager, bloomUpHeapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    // Blue noise tile sampler. Re-derived here rather than read from
    // blueNoiseSamplerInfo for the same reason clampInfo is: it keeps
    // sampler-slot init independent of texture-init ordering.
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
    // rtr_half.slang declares smp and blueNoiseSampler. The pipeline builds only
    // when the RT context exists; with empty bindings this is a no-op.
    Vk::InitHeapPassSamplers<Shaders::RtrHalf>(heapManager, rtrHalfHeapBindings, Vk::SamplerSlot<"smp">(defaultInfo), Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseInfo));
    Vk::InitHeapPassSamplers<Shaders::Taa>(heapManager, taaPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::Fxaa>(heapManager, fxaaPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::Mlaa>(heapManager, mlaaPass.heapBindings, Vk::SamplerSlot<"sPoint">(defaultInfo));
    // SMAA's EDGE module is the only one that samples pointSampler; WEIGHT and
    // BLEND use linearSampler only.
    Vk::InitHeapPassSamplers<Shaders::SmaaEdge>(heapManager, smaaEdgePass.heapBindings, Vk::SamplerSlot<"pointSampler">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::SmaaWeight>(heapManager, smaaWeightPass.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::SmaaBlend>(heapManager, smaaBlendPass.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::Blit>(heapManager, blitPass.heapBindings, Vk::SamplerSlot<"smp">(defaultInfo));
    Vk::InitHeapPassSamplers<Shaders::VolumetricTemporal>(heapManager, volumetricTemporalPass.heapBindings, Vk::SamplerSlot<"linearSampler">(defaultInfo));
    const VkSamplerCreateInfo repeatInfo = Vk::SamplerBuilder {}.Linear().Repeat().LodRange(0.0F, 0.0F).Info();
    Vk::InitHeapPassSamplers<Shaders::VolumetricFogInject>(heapManager, volumetricFogInjectPass.heapBindings, Vk::SamplerSlot<"noiseSampler">(repeatInfo));
    Vk::InitHeapPassSamplers<Shaders::VolumetricLightInject>(heapManager, volumetricLightInjectPass.heapBindings, Vk::SamplerSlot<"shadowSampler">(shadowInfo));
}

auto RenderContext::Impl::InitSkeletalAnimationResources() -> std::expected<void, ErrorCode> {
    JPH::Array<JPH::Mat44> identities(8192, JPH::Mat44::sIdentity());
    for (int i = 0; i < 2; ++i) {
        auto jb_res = Vk::Buffer::Create(
            allocator.Get(), sizeof(JPH::Mat44) * 8192, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
            Vk::MemoryUsage::CPUToGPU
        );
        if (!jb_res) {
            return std::unexpected(ErrorCode(jb_res.error()));
        }
        frames.jointBuffers[i] = std::move(*jb_res);

        auto mapped = frames.jointBuffers[i].Map();
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
    return {};
}

auto RenderContext::Impl::InitLightingLUTs() -> std::expected<void, ErrorCode> {
    stagingContext = std::make_unique<Vk::StagingContext>(allocator, ctx);

    using namespace Resource;
    const size_t matRawSize = ltc_mat.size() - 128;
    const size_t ampRawSize = ltc_amp.size() - 128;

    return stagingContext->Begin()
        .and_then([&]() -> std::expected<Vk::IBLPayload, ZHLN::ErrorCode> {
            return Vk::IBLProcessor::Bake(*this);
        })
        .and_then([&, matRawSize, ampRawSize](auto&& ibl) -> auto {
            iblPayload = std::forward<decltype(ibl)>(ibl);
            ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

            return Vk::Buffer::Create(allocator.Get(), matRawSize + ampRawSize, Vk::BufferUsage::TransferSrc, Vk::MemoryUsage::CPUOnly)
                .transform_error([](auto res) -> ErrorCode { return res; });
        })
        .and_then([&, matRawSize](auto&& ltcStaging) -> auto {
            constexpr auto kLtcUsage = Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled;
            auto           makeLtc   = [&] {
                return Vk::ImageBuilder {}.Texture2D(64, 64, VK_FORMAT_R16G16B16A16_SFLOAT, kLtcUsage, 1).Build(allocator.Get());
            };

            return makeLtc()
                .transform_error([](auto res) -> ErrorCode { return res; })
                .and_then([&, ltcStaging = std::forward<decltype(ltcStaging)>(ltcStaging), matRawSize, makeLtc](auto&& matImg) mutable -> auto {
                    return makeLtc()
                        .transform_error([](auto res) -> ErrorCode { return res; })
                        .transform(
                            [&, matImg = std::forward<decltype(matImg)>(matImg), ltcStaging = std::move(ltcStaging),
                             matRawSize](auto&& ampImg) mutable -> auto {
                                stagingContext->UploadImage2DBuffer(matImg.Handle(), 64, 64, 1, ltcStaging.Handle(), 0);
                                stagingContext->UploadImage2DBuffer(ampImg.Handle(), 64, 64, 1, ltcStaging.Handle(), matRawSize);

                                stagingContext->AddBuffer(std::move(ltcStaging));
                                return std::make_pair(std::move(matImg), std::forward<decltype(ampImg)>(ampImg));
                            }
                        );
                });
        })
        .and_then([&](auto&& images) -> std::expected<void, ErrorCode> {
            ltcMatImage = std::move(images.first);
            ltcAmpImage = std::move(images.second);

            stagingContext->ExecuteAsync();

            return Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcMatImage.Handle())
                .transform_error([](auto res) -> ErrorCode { return res; })
                .and_then([&](auto&& matView) -> std::expected<void, ErrorCode> {
                    ltcMatView = std::forward<decltype(matView)>(matView);
                    return Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcAmpImage.Handle())
                        .transform_error([](auto res) -> ErrorCode { return res; })
                        .transform([&](auto&& ampView) -> auto {
                            ltcAmpView     = std::forward<decltype(ampView)>(ampView);
                            ltcMatViewInfo = Vk::MakeViewCreateInfo2D(ltcMatImage.Handle(), VK_FORMAT_R16G16B16A16_SFLOAT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                            ltcAmpViewInfo = Vk::MakeViewCreateInfo2D(ltcAmpImage.Handle(), VK_FORMAT_R16G16B16A16_SFLOAT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                            ApplyImageDebugNames(*this);
                        });
                });
        });
}

auto RenderContext::Impl::AdoptBindlessTexture(Vk::Image&& image, Vk::ImageView&& view, VkFormat format, uint32_t mipLevels, bool cube)
    -> std::expected<uint32_t, ErrorCode> {
    // globalTextures[] is addressed by raw offset (textureHeapBase + index),
    // not through SlotAllocator, so nothing else bounds this counter: an overrun
    // would spill into the frame partition that follows the array and quietly
    // rewrite a pass's descriptors.
    //
    // A slot handed back by ReleaseBindlessTexture is reused before the counter
    // advances, so exhaustion now means 32768 slots are genuinely occupied at
    // once rather than that a caller has been recreating textures. Recoverable,
    // so it is an error rather than an assertion: every caller already
    // substitutes the white fallback for a texture it could not create.
    uint32_t index = 0;
    if (!freeTextureIndices.empty()) {
        index = freeTextureIndices.back();
        freeTextureIndices.pop_back();
    } else {
        if (nextTextureIndex >= kGlobalTextureSlots) [[unlikely]] {
            ZHLN::Log("[Bindless] globalTextures[] exhausted: all {} slots are occupied. Refusing the upload.", kGlobalTextureSlots);
            // image and view die with this scope: the refusal costs the GPU
            // allocation that was already made, but leaks nothing.
            return std::unexpected(Vk::DescriptorHeapError::ResourceSlotsExhausted);
        }
        index = nextTextureIndex++;
    }

    // The arrays are slot-indexed rather than append-only: a recycled index is
    // not necessarily the highest one ever handed out.
    if (textureImages.size() <= index) {
        textureImages.resize(static_cast<size_t>(index) + 1);
        textureViews.resize(static_cast<size_t>(index) + 1);
    }

    WriteTextureSlotToHeap(index, image.Handle(), format, mipLevels, cube);
    textureImages[index] = std::move(image);
    textureViews[index]  = std::move(view);
    return index;
}

void RenderContext::Impl::ReleaseBindlessTexture(uint32_t bindlessIndex) noexcept {
    // Black/white/normal are what every failed lookup resolves to and what a
    // released slot is pointed at on reclamation, so they stay resident.
    if (bindlessIndex <= kFallbackNormalTextureIndex || bindlessIndex >= textureImages.size()) [[unlikely]] {
        return;
    }
    if (!textureImages[bindlessIndex].Valid()) {
        // Never handed out, or already awaiting reclamation: releasing twice
        // would let one index back two live textures.
        return;
    }

    // The descriptor keeps pointing at this slot until reclamation -- in-flight
    // frames may still be sampling it -- so ownership of the image and view
    // moves into the pending entry instead of dying here.
    pendingTextureFrees[presenter.frameIndex].push_back(
        ReleasedTextureSlot {.index = bindlessIndex, .image = std::move(textureImages[bindlessIndex]), .view = std::move(textureViews[bindlessIndex])}
    );
}

void RenderContext::Impl::ReclaimTextureSlots(uint32_t frameIndex) noexcept {
    auto& pending = pendingTextureFrees[frameIndex];
    for (auto& released: pending) {
        // BeginFrame has already waited on the other parity's fence, so the
        // queue is idle: rewriting the descriptor cannot race a reader. Point
        // the slot at the white fallback -- created 1x1 sRGB in
        // InitializeSystemTextures -- so a stale index still baked into an
        // instance or material resolves to white rather than to the image that
        // is destroyed here.
        WriteTextureSlotToHeap(released.index, textureImages[kFallbackWhiteTextureIndex].Handle(), VK_FORMAT_R8G8B8A8_SRGB, 1, false);
        freeTextureIndices.push_back(released.index);
    }
    // Dropping the entries releases the images and views of every slot that was
    // not handed out again. Nothing is in flight, so no deletion queue is
    // needed for them.
    pending.clear();
}

auto RenderContext::Impl::InitBakeHeapBindings() noexcept -> std::expected<void, ErrorCode> {
    // One shared binding table for every one-shot compute bake (SMAA / BRDF /
    // IBL specular / procedural). Each bake calls BeginImmediate and writes
    // fresh blocks into the immediate partition: ExecuteImmediate is
    // synchronous, so a rewound partition can never hold descriptors the GPU is
    // still reading.
    const auto shader = Vk::CreateShaderDesc<Shaders::Modules::ProceduralBakeCS>();
    if (!proceduralBakeDescLayout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, proceduralBakeDescLayout.sets[0], 0, Vk::kHeapPushDataLayout.heapIndexOffset, Vk::HeapLifecycle::Immediate, bakeHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }
    return {};
}

void RenderContext::Impl::WriteTextureSlotToHeap(uint32_t bindlessIndex, VkImage image, VkFormat format, uint32_t mipLevels, bool cube) noexcept {
    // The globalTextures[] array is pinned to a contiguous heap region by the
    // binding-11 mapping; index N lives at slot (textureHeapBase + N).
    Vk::TextureHandle           slot {textureHeapBase + bindlessIndex};
    const VkImageViewCreateInfo info = cube ? Vk::MakeViewCreateInfoCube(image, format, mipLevels) :
                                              Vk::MakeViewCreateInfo2D(image, format, mipLevels, VK_IMAGE_ASPECT_COLOR_BIT);
    heapManager.WriteImage(slot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

auto RenderContext::Impl::InitializeSystemTextures() noexcept -> std::expected<void, ErrorCode> {
    ZHLN::Log("[Resource Factory] Registering fallback system texture slots...");

    std::array<uint8_t, 4> blackPixel  = {0, 0, 0, 0};
    std::array<uint8_t, 4> whitePixel  = {255, 255, 255, 255};
    std::array<uint8_t, 4> normalPixel = {128, 128, 255, 255};

    return CreateTextureInternal(blackPixel.data(), 1, 1, false).and_then([&, whitePixel, normalPixel](uint32_t blackIdx) -> std::expected<void, ErrorCode> {
        return CreateTextureInternal(whitePixel.data(), 1, 1, true).and_then([&, blackIdx, normalPixel](uint32_t whiteIdx) -> std::expected<void, ErrorCode> {
            return CreateTextureInternal(normalPixel.data(), 1, 1, false).and_then([&, blackIdx, whiteIdx](uint32_t normalIdx) -> std::expected<void, ErrorCode> {
                if (blackIdx != kFallbackBlackTextureIndex || whiteIdx != kFallbackWhiteTextureIndex || normalIdx != kFallbackNormalTextureIndex) {
                    return std::unexpected(BindlessSetupError::DefaultTextureRegistrationFailed);
                }
                return {};
            });
        });
    });
}

} // namespace ZHLN
