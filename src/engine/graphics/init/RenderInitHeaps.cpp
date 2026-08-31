// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitHeaps.cpp
#include "../IBLProcessor.hpp"
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include <StagingContext.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstring>

namespace ZHLN {

// Private bindless/heap setup failure (Tier 1): declared at file scope in this
// translation unit so no header exposes it.
enum class BindlessSetupError : uint8_t {
    DefaultTextureRegistrationFailed[[= ZHLN::Description<"Default bindless texture registration returned unexpected indices">{}]] = 1,
};

auto RenderContext::Impl::InitBindless() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    // Reflect the authoritative GlobalSceneRegistry layout out of the compiled
    // scene shaders. The union across every `scene`-consuming entry point
    // (basic VS/PS, forward PS, punctual-shadow VS) covers exactly the registry
    // members in live use: {0,1,2,3,4,5,6,10,11}. Under the descriptor-heap
    // model the reflection no longer produces descriptor set layouts — it only
    // reports which set-0 bindings exist, and the engine maps them onto the
    // heaps below (see BuildSceneHeapMappings).
    auto basicShaders = Resource::GetShaderProgram(Basic);
    return LoadAndCreateShaders(
               {.path = Resource::Paths::BasicVS, .fallback = basicShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::BasicPS, .fallback = basicShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& basicStages) -> std::expected<void, Error> {
            const Vk::ReflectedStageInput reflectInputs[6] = {
                {.shader = Vk::CreateShaderDesc(basicStages.GetVertSpv()), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc(basicStages.GetFragSpv()), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                {.shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(PunctualShadows).vertex), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc(Resource::forward_frag), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                // Compute consumers widen the stage flags of the members they
                // touch (`scene.frame` for both particle simulations). Without
                // them the union reflection would only carry VS|FS stages and
                // the compute-side mappings would be incomplete.
                {.shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ParticleUpdate).vertex), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
                {.shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(MeshParticleUpdate).vertex), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
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
        .and_then([&]() -> std::expected<void, Error> {
            // Build the samplers first: their VkSamplerCreateInfo values are
            // what vkWriteSamplerDescriptorsEXT consumes for the sampler heap.
            auto globalBuilder =
                Vk::SamplerBuilder {}.Linear().Repeat().Anisotropy(ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy).LodRange(0.0f, 0.0f);
            auto clampBuilder = Vk::SamplerBuilder {}.Linear().ClampToEdge();

            return globalBuilder.Build(ctx.Device())
                .transform_error([](auto err) -> Error { return err; })
                .and_then([&](auto&& globalRes) -> std::expected<void, Error> {
                    globalSampler = std::forward<decltype(globalRes)>(globalRes);
                    return clampBuilder.Build(ctx.Device())
                        .transform_error([](auto err) -> Error { return err; })
                        .and_then([&](auto&& clampRes) -> std::expected<void, Error> {
                            clampSampler = std::forward<decltype(clampRes)>(clampRes);
                            return InitSceneHeaps(globalBuilder.Info(), clampBuilder.Info());
                        });
                });
        })
        .and_then([&]() -> std::expected<void, Error> { return InitBakeHeapBindings(); })
        .and_then([&]() -> std::expected<void, Error> { return InitSkeletalAnimationResources(); })
        .and_then([&]() -> std::expected<void, Error> { return InitLightingLUTs(); })
        .and_then([&]() -> std::expected<void, Error> { return InitializeSystemTextures(); })
        .and_then([&]() -> std::expected<void, Error> { return InitializeBlueNoiseTexture(); })
        .and_then([&]() -> std::expected<void, Error> {
            // IBL images exist after InitLightingLUTs; write their heap
            // descriptors once (they never change after init). The translucent
            // lighting + decal depth descriptors are (re)written whenever the
            // targets are recreated.
            WriteSceneStaticImageDescriptors();
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            ZHLN::Log("[RenderInit] Pre-allocating persistently mapped Double-Buffered Debug VBOs...");
            size_t bufferSize = kMaxDebugVertices * (sizeof(VertexPosition) + sizeof(VertexAttributes));
            for (int i = 0; i < 2; ++i) {
                auto gpu_buf_res = Vk::Buffer::Create(
                    allocator.Get(), bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
                );
                if (!gpu_buf_res) {
                    return std::unexpected(Error(gpu_buf_res.error()));
                }
                auto gpu_buf = std::move(*gpu_buf_res);

                auto address               = ctx.BufferAddress(gpu_buf.Handle());
                frames.debugMeshHandles[i] = meshPool.Create(std::move(gpu_buf), kMaxDebugVertices, address);
            }
            return {};
        });
}

auto RenderContext::Impl::InitSceneHeaps(const VkSamplerCreateInfo& globalSamplerInfo, const VkSamplerCreateInfo& clampSamplerInfo) noexcept
    -> std::expected<void, Error> {
    auto reflectedPushLayout = Vk::ReflectHeapPushDataLayout(Resource::gpu_abi_comp.data(), Resource::gpu_abi_comp.size());
    if (!reflectedPushLayout) [[unlikely]] {
        return std::unexpected(reflectedPushLayout.error());
    }
    if (reflectedPushLayout->frameAddressOffsets.front() < Vk::kScenePassPushPayloadBytes) [[unlikely]] {
        return std::unexpected(Vk::SpirvLayoutError::HeapPushOverlapsPassData);
    }
    heapPushDataLayout = *reflectedPushLayout;

    auto init_res = heapManager.Init(
        ctx, allocator, kSceneStaticResourceSlots + kGlobalTextureSlots + kPassStaticResourceSlots, kSceneDynamicResourceSlots,
        kSceneStaticSamplerSlots + kPassStaticSamplerSlots, kSceneDynamicSamplerSlots, 2
    );
    if (!init_res) {
        return std::unexpected(init_res.error());
    }

    // Slang is the layout authority for the frame-address fields and the
    // per-dispatch descriptor index. Reject devices whose push-data budget
    // cannot fit the reflected layout.
    if (heapManager.PushDataMaxSize() < heapPushDataLayout.requiredSize) [[unlikely]] {
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
    textureHeapBase    = kSceneStaticResourceSlots; // globalTextures[] region starts after the static slots

    // Advance the allocator cursors past the offset-addressed regions:
    //   resource heap: [scene static 16) [globalTextures 32768) [pass slots ...)
    //   sampler heap:  [scene static 16) [pass sampler slots ...)
    //
    // The skips assume exactly the scene allocations above; anything else
    // allocating before this point would silently overlap the texture region.
    if (heapManager.StaticResourceCursor() != 4 || heapManager.StaticSamplerCursor() != 3) [[unlikely]] {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    heapManager.SkipStaticResourceSlots(kGlobalTextureSlots + (kSceneStaticResourceSlots - 4));
    heapManager.SkipStaticSamplerSlots(kSceneStaticSamplerSlots - 3);

    // --- Write the static sampler descriptors into the sampler heap ---
    heapManager.WriteSampler(globalSamplerSlot, globalSamplerInfo);
    heapManager.WriteSampler(clampSamplerSlot, clampSamplerInfo);
    // pointSamplerSlot is written by WritePointSamplerToHeap once the sampler exists.

    // --- Bake the set/binding -> heap mapping tables for pipeline creation ---
    BuildSceneHeapMappings();

    return {};
}

void RenderContext::Impl::BuildSceneHeapMappings() noexcept {
    // May run more than once (initial bake + decal-pipeline bake after the
    // decal reflection exists), so rebuild both tables from scratch.
    sceneHeapMappings.entries.clear();
    decalSceneHeapMappings.entries.clear();

    // GlobalSceneRegistry (common.slang) member order -> binding numbers:
    //   0 defaultSampler    4 g_joints        8 brdfLUT
    //   1 frame             5 g_prevJoints    9 clampSampler
    //   2 lights            6 g_morphDeltas  10 texTransLighting
    //   3 g_instances       7 prefilteredMap 11 globalTextures[]
    //
    // Per-frame buffers (1..6) use VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT.
    // Their push-data offsets come from DescriptorHeapPushData's Slang layout;
    // images and samplers sit in static heap slots via
    // VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT.
    const auto add_scene_set = [&](uint32_t setIndex, HeapMappingSet& out) -> void {
        using enum VkDescriptorMappingSourceEXT;
        const auto& set = (setIndex == 0) ? bindlessLayout.reflectedSets[0] : decalDescLayout.reflectedSets[setIndex];

        for (const auto& b: set.bindings) {
            VkDescriptorSetAndBindingMappingEXT entry = {
                .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
                .pNext         = nullptr,
                .descriptorSet = setIndex,
                .firstBinding  = b.binding,
                .bindingCount  = 1,
                .resourceMask  = 0,
                .source        = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
                .sourceData    = {},
            };

            switch (b.binding) {
                case 0: // defaultSampler
                    entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
                    entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heapManager.SamplerOffset(globalSamplerSlot.index));
                    break;
                case 1: // frame (uniform buffer)
                    entry.resourceMask                 = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
                    entry.source                       = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
                    entry.sourceData.pushAddressOffset = heapPushDataLayout.frameAddressOffsets[0];
                    break;
                case 2: // lights
                case 3: // g_instances
                case 4: // g_joints
                case 5: // g_prevJoints
                case 6: // g_morphDeltas
                    entry.resourceMask                 = VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT;
                    entry.source                       = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
                    entry.sourceData.pushAddressOffset = heapPushDataLayout.frameAddressOffsets[b.binding - 1];
                    break;
                case 7: // prefilteredMap
                    entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
                    entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heapManager.ResourceOffset(iblPrefilteredSlot.index));
                    break;
                case 8: // brdfLUT
                    entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
                    entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heapManager.ResourceOffset(iblBrdfLutSlot.index));
                    break;
                case 9: // clampSampler
                    entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
                    entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heapManager.SamplerOffset(clampSamplerSlot.index));
                    break;
                case 10: // texTransLighting
                    entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
                    entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heapManager.ResourceOffset(transLightingSlot.index));
                    break;
                case 11: // globalTextures[] - the bindless texture array
                    entry.resourceMask                              = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
                    entry.sourceData.constantOffset.heapOffset      = static_cast<uint32_t>(heapManager.ResourceOffset(textureHeapBase));
                    entry.sourceData.constantOffset.heapArrayStride = static_cast<uint32_t>(heapManager.ResourceStride());
                    break;
                default:
                    continue; // Unknown binding: nothing to map
            }

            out.entries.push_back(entry);
        }
        out.Finalize();
    };

    add_scene_set(0, sceneHeapMappings);
    add_scene_set(1, decalSceneHeapMappings);
}

void RenderContext::Impl::BuildDecalHeapMappings() noexcept {
    // Re-run the scene mapping bake: at initial init time decalDescLayout had
    // not been reflected yet, so the decal's scene-subset (set 1) entries are
    // empty. After reflection this picks them up.
    BuildSceneHeapMappings();

    // decal.slang set 0: {binding 0 = texDepth (sampled image), binding 1 = pointSampler}.
    decalHeapMappings.entries.clear();
    for (const auto& b: decalDescLayout.reflectedSets[0].bindings) {
        VkDescriptorSetAndBindingMappingEXT entry = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext         = nullptr,
            .descriptorSet = 0,
            .firstBinding  = b.binding,
            .bindingCount  = 1,
            .resourceMask  = 0,
            .source        = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
            .sourceData    = {},
        };
        switch (b.binding) {
            case 0: // texDepth
                entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
                entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heapManager.ResourceOffset(decalDepthSlot.index));
                break;
            case 1: // pointSampler
                entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
                entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heapManager.SamplerOffset(pointSamplerSlot.index));
                break;
            default:
                continue;
        }
        decalHeapMappings.entries.push_back(entry);
    }
    decalHeapMappings.Finalize();
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
    // pipeline-build time). Sampler ORDER per pass mirrors each pass's set-0
    // declaration order (sampler positions only).
    const VkSamplerCreateInfo defaultInfo = defaultSamplerInfo;
    const VkSamplerCreateInfo pointInfo   = pointSamplerInfo;
    const VkSamplerCreateInfo shadowInfo  = shadowSamplerInfo;
    const VkSamplerCreateInfo clampInfo   = [&]() -> VkSamplerCreateInfo {
        // clampSampler is the linear clamp-to-edge sampler; its create info was
        // captured at InitBindless time (kept in the heap slot already) — for
        // pass slots we re-derive it identically.
        return Vk::SamplerBuilder {}.Linear().ClampToEdge().Info();
    }();

    {
        std::array<VkSamplerCreateInfo, 1> infos = {pointInfo};
        Vk::InitHeapPassSamplers(heapManager, hizHeapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, cullingHeapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 1> infos = {defaultInfo};
        Vk::InitHeapPassSamplers(heapManager, bloomThresholdHeapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, bloomDownHeapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, bloomUpHeapBindings, infos);
    }
    // Blue noise tile sampler, appended last to match the tail declaration
    // position in lighting.slang / reflection.slang. Re-derived here rather
    // than read from blueNoiseSamplerInfo for the same reason clampInfo is:
    // it keeps sampler-slot init independent of texture-init ordering.
    const VkSamplerCreateInfo blueNoiseInfo = Vk::SamplerBuilder {}.Nearest().Repeat().LodRange(0.0F, 0.0F).Info();
    {
        std::array<VkSamplerCreateInfo, 5> infos = {defaultInfo, shadowInfo, clampInfo, pointInfo, blueNoiseInfo};
        Vk::InitHeapPassSamplers(heapManager, lightingPass.heapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 4> infos = {defaultInfo, pointInfo, clampInfo, blueNoiseInfo};
        Vk::InitHeapPassSamplers(heapManager, reflectionPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, translucentReflectionPass.heapBindings, infos);
    }
    {
        // rtr_half.slang declares exactly two samplers, smp and
        // blueNoiseSampler, in that order. The pipeline builds only when the
        // RT context exists; with empty bindings this is a no-op.
        std::array<VkSamplerCreateInfo, 2> infos = {defaultInfo, blueNoiseInfo};
        Vk::InitHeapPassSamplers(heapManager, rtrHalfHeapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 1> infos = {defaultInfo};
        Vk::InitHeapPassSamplers(heapManager, taaPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, fxaaPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, mlaaPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, smaaEdgePass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, smaaWeightPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, smaaBlendPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, blitPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, volumetricTemporalPass.heapBindings, infos);
    }
    {
        const VkSamplerCreateInfo repeatInfo = Vk::SamplerBuilder {}.Linear().Repeat().LodRange(0.0F, 0.0F).Info();
        std::array<VkSamplerCreateInfo, 1> infos = {repeatInfo};
        Vk::InitHeapPassSamplers(heapManager, volumetricFogInjectPass.heapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 1> infos = {shadowInfo};
        Vk::InitHeapPassSamplers(heapManager, volumetricLightInjectPass.heapBindings, infos);
    }
}

auto RenderContext::Impl::InitSkeletalAnimationResources() -> std::expected<void, Error> {
    JPH::Array<JPH::Mat44> identities(8192, JPH::Mat44::sIdentity());
    for (int i = 0; i < 2; ++i) {
        auto jb_res = Vk::Buffer::Create(
            allocator.Get(), sizeof(JPH::Mat44) * 8192, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        if (!jb_res) {
            return std::unexpected(Error(jb_res.error()));
        }
        frames.jointBuffers[i] = std::move(*jb_res);

        auto mapped = frames.jointBuffers[i].Map();
        std::memcpy(mapped.data, identities.data(), identities.size() * sizeof(JPH::Mat44));
    }

    auto mdb_res = Vk::Buffer::Create(
        allocator.Get(), sizeof(float) * 4 * 1000000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU
    );
    if (!mdb_res) {
        return std::unexpected(Error(mdb_res.error()));
    }
    morphDeltasBuffer = std::move(*mdb_res);
    return {};
}

auto RenderContext::Impl::InitLightingLUTs() -> std::expected<void, Error> {
    stagingContext = std::make_unique<Vk::StagingContext>(allocator, ctx);

    using namespace Resource;
    const size_t matRawSize = ltc_mat.size() - 128;
    const size_t ampRawSize = ltc_amp.size() - 128;

    return stagingContext->Begin()
        .and_then([&]() -> std::expected<Vk::IBLPayload, ZHLN::Error> { return Vk::IBLProcessor::Bake(*this); })
        .and_then([&, matRawSize, ampRawSize](auto&& ibl) -> auto {
            iblPayload = std::forward<decltype(ibl)>(ibl);
            ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

            return Vk::Buffer::Create(allocator.Get(), matRawSize + ampRawSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
                .transform_error([](auto res) -> Error { return res; });
        })
        .and_then([&, matRawSize](auto&& ltcStaging) -> auto {
            const VkImageCreateInfo ltcInfo = {
                .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext                 = {},
                .flags                 = {},
                .imageType             = VK_IMAGE_TYPE_2D,
                .format                = VK_FORMAT_R16G16B16A16_SFLOAT,
                .extent                = {.width = 64, .height = 64, .depth = 1},
                .mipLevels             = 1,
                .arrayLayers           = 1,
                .samples               = VK_SAMPLE_COUNT_1_BIT,
                .tiling                = VK_IMAGE_TILING_OPTIMAL,
                .usage                 = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = {},
                .pQueueFamilyIndices   = {},
                .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
            };

            return Vk::Image::Create(allocator.Get(), ltcInfo, VMA_MEMORY_USAGE_GPU_ONLY)
                .transform_error([](auto res) -> Error { return res; })
                .and_then([&, ltcInfo, ltcStaging = std::forward<decltype(ltcStaging)>(ltcStaging), matRawSize](auto&& matImg) mutable -> auto {
                    return Vk::Image::Create(allocator.Get(), ltcInfo, VMA_MEMORY_USAGE_GPU_ONLY)
                        .transform_error([](auto res) -> Error { return res; })
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
        .and_then([&](auto&& images) -> std::expected<void, Error> {
            ltcMatImage = std::move(images.first);
            ltcAmpImage = std::move(images.second);

            stagingContext->ExecuteAsync();

            return Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcMatImage.Handle())
                .transform_error([](auto res) -> Error { return res; })
                .and_then([&](auto&& matView) -> std::expected<void, Error> {
                    ltcMatView = std::forward<decltype(matView)>(matView);
                    return Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcAmpImage.Handle())
                        .transform_error([](auto res) -> Error { return res; })
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
    -> std::expected<uint32_t, Error> {
    // globalTextures[] is addressed by raw offset (textureHeapBase + index),
    // not through SlotAllocator, so nothing else bounds this counter. Slot
    // kGlobalTextureSlots is the first slot of the *pass* region that follows
    // it in the same heap, so an overrun would quietly rewrite another pass's
    // descriptors long before it ran off the end of the buffer.
    //
    // Nothing ever gives a slot back -- Unload is deliberately a no-op and the
    // renderer keeps every texture resident for the life of the device -- so
    // exhaustion means a caller is recreating textures in a loop rather than
    // that 32768 distinct textures are genuinely in use. Recoverable, so it is
    // an error rather than an assertion: every caller already substitutes the
    // white fallback for a texture it could not create.
    if (nextTextureIndex >= kGlobalTextureSlots) [[unlikely]] {
        ZHLN::Log("[Bindless] globalTextures[] exhausted: all {} slots taken. Refusing the upload; a caller is leaking texture slots.", kGlobalTextureSlots);
        // image and view die with this scope: the refusal costs the GPU
        // allocation that was already made, but leaks nothing.
        return std::unexpected(Vk::DescriptorHeapError::ResourceSlotsExhausted);
    }

    const uint32_t index = nextTextureIndex++;
    WriteTextureSlotToHeap(index, image.Handle(), format, mipLevels, cube);
    textureImages.push_back(std::move(image));
    textureViews.push_back(std::move(view));
    return index;
}

auto RenderContext::Impl::InitBakeHeapBindings() noexcept -> std::expected<void, Error> {
    // One shared storage-image slot span for every one-shot compute bake
    // (SMAA / BRDF / IBL specular / procedural). ExecuteImmediate is
    // synchronous, so the same slots are rewritten per bake.
    const auto shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::ProceduralBakeComp).vertex, "CSMain");
    if (!proceduralBakeDescLayout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    Vk::BuildHeapPassBindings(
        heapManager, proceduralBakeDescLayout.reflectedSets[0], 0, heapPushDataLayout.heapIndexOffset, kBakeHeapSlotSpan, bakeHeapBindings
    );
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

auto RenderContext::Impl::InitializeSystemTextures() noexcept -> std::expected<void, Error> {
    ZHLN::Log("[Resource Factory] Registering fallback system texture slots...");

    std::array<uint8_t, 4> blackPixel  = {0, 0, 0, 0};
    std::array<uint8_t, 4> whitePixel  = {255, 255, 255, 255};
    std::array<uint8_t, 4> normalPixel = {128, 128, 255, 255};

    return CreateTextureInternal(blackPixel.data(), 1, 1, false).and_then([&, whitePixel, normalPixel](uint32_t blackIdx) -> std::expected<void, Error> {
        return CreateTextureInternal(whitePixel.data(), 1, 1, true).and_then([&, blackIdx, normalPixel](uint32_t whiteIdx) -> std::expected<void, Error> {
            return CreateTextureInternal(normalPixel.data(), 1, 1, false).and_then([&, blackIdx, whiteIdx](uint32_t normalIdx) -> std::expected<void, Error> {
                if (blackIdx != kFallbackBlackTextureIndex || whiteIdx != kFallbackWhiteTextureIndex || normalIdx != kFallbackNormalTextureIndex) {
                    return std::unexpected(BindlessSetupError::DefaultTextureRegistrationFailed);
                }
                return {};
            });
        });
    });
}

} // namespace ZHLN
