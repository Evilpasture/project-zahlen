// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderInit.cpp
#include "../TTYBackend.hpp"
#include "IBLProcessor.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "SMAALUTGenerator.hpp"
#include "backends/imgui_impl_glfw.h"
#include "imgui.h"
#include "imgui_impl_vulkan_heap.h"
#include <Features.hpp>
#include <StagingContext.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <stb_image.h>
#include <vector>
namespace {

struct HardwareCaps {
    bool supportsDrawIndirectCount = false;
    bool supportsInt64             = false;
    // VK_EXT_mesh_shader: extension + features + the hardware limits the
    // Zahlen task/mesh shaders were authored against.
    bool supportsMeshShader = false;
    // Requested separately: FeatureChain::Optional drops the WHOLE feature
    // struct when any single requested bit is unsupported, so asking for
    // multiviewMeshShader unconditionally would silently disable taskShader
    // and meshShader too on a device that lacks only the multiview bit.
    bool supportsMultiviewMeshShader = false;
};

class HardwareCapsProber {
  public:
    explicit HardwareCapsProber(VkPhysicalDevice physicalDevice, uint32_t apiVersion) noexcept: _physicalDevice(physicalDevice), _apiVersion(apiVersion) {
    }

    auto ProbeInt64(bool& target) && noexcept -> HardwareCapsProber&& {
        VkPhysicalDeviceFeatures2 features2 {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
        target = (features2.features.shaderInt64 == VK_TRUE);
        return std::move(*this);
    }

    auto ProbeDrawIndirectCount(bool& target) && noexcept -> HardwareCapsProber&& {
        bool hasExt = ZHLN::Vk::IsDeviceExtensionSupported(_physicalDevice, "VK_KHR_draw_indirect_count");
        if (hasExt || _apiVersion >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceFeatures2 features2 {};

            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            VkPhysicalDeviceVulkan12Features features12 {};
            features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            features2.pNext  = &features12;
            vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
            target = (features12.drawIndirectCount == VK_TRUE);
        } else {
            target = false;
        }
        return std::move(*this);
    }

  private:
    VkPhysicalDevice _physicalDevice;
    uint32_t         _apiVersion;
};

bool CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept;
bool CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept;

HardwareCaps ProbeHardware(VkPhysicalDevice physicalDevice, uint32_t apiVersion) noexcept {
    HardwareCaps caps {};
    HardwareCapsProber(physicalDevice, apiVersion).ProbeInt64(caps.supportsInt64).ProbeDrawIndirectCount(caps.supportsDrawIndirectCount);
    caps.supportsMeshShader          = CheckMeshShaderSupport(physicalDevice);
    caps.supportsMultiviewMeshShader = caps.supportsMeshShader && CheckMultiviewMeshShaderSupport(physicalDevice);
    return caps;
}

// VK_EXT_mesh_shader is only usable when the extension is present, the two
// feature bits are advertised AND the device's mesh-shader limits cover the
// geometry budget baked into basic_task.slang / basic_mesh.slang. Anything
// less and the engine silently keeps the vertex pipeline.
bool CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept {
    if (!ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
        // Log the count too: a suspiciously round number here (128, 256...)
        // means something is truncating the enumeration again.
        ZHLN::Log(
            "[RenderInit] VK_EXT_mesh_shader not present among the {} device extensions reported; using the vertex pipeline.",
            ZHLN::Vk::EnumerateDeviceExtensions(physicalDevice).size()
        );
        return false;
    }

    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    if (meshFeatures.taskShader != VK_TRUE || meshFeatures.meshShader != VK_TRUE) {
        ZHLN::Log(
            "[RenderInit] VK_EXT_mesh_shader present but its features are not advertised (taskShader={}, meshShader={}); using the vertex pipeline.",
            meshFeatures.taskShader, meshFeatures.meshShader
        );
        return false;
    }

    const ZHLN_MeshShaderLimits limits = ZHLN_QueryMeshShaderLimits(physicalDevice);
    if (!ZHLN_MeshShaderLimitsSufficient(&limits)) {
        ZHLN::Log(
            "[RenderInit] VK_EXT_mesh_shader present but limits are insufficient "
            "(maxMeshOutputVertices={}, maxMeshOutputPrimitives={}, maxTaskWorkGroupInvocations={}); using the vertex pipeline.",
            limits.max_mesh_output_vertices, limits.max_mesh_output_primitives, limits.max_task_work_group_invocations
        );
        return false;
    }

    // Deliberately silent on success: a working feature is not news. Every
    // return false above explains itself.
    return true;
}

bool CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept {
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return meshFeatures.multiviewMeshShader == VK_TRUE;
}

bool CheckRayTracingSupport(VkPhysicalDevice physicalDevice) noexcept {
    return ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
           ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
           ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
}

} // namespace

namespace ZHLN {

std::expected<void, Error> RenderContext::Impl::BuildParticlePipelines() {
    using enum Resource::ShaderID;

    // 1. Allocate global default particle buffer to prevent null vkGetBufferDeviceAddress crashes
    size_t particleBufferSize = RenderContext::Impl::kGpuParticleCount * sizeof(Particle);
    auto   pb_res             = Vk::Buffer::Create(
        allocator.Get(), particleBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );
    if (!pb_res) {
        ZHLN::Log("ERROR: Failed to allocate global particle buffer!");
        return std::unexpected(pb_res.error());
    }
    particleBuffer = std::move(*pb_res);

    // 2. Build GPU Compute Simulation Pipeline (particle_update.hlsl)
    //    VK_EXT_descriptor_heap: `scene.frame` reads via the PUSH_ADDRESS
    //    mapping; per-dispatch data travels through vkCmdPushDataEXT.
    auto csShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ParticleUpdate).vertex);

    if (!particleUpdatePass.BuildHeap(ctx.Device(), csShader, &sceneHeapMappings.info)) {
        ZHLN::Log("ERROR: Failed to build particle update compute pipeline!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // 3. Build Billboard Graphics Pipeline (particle_render.hlsl)
    particleRenderLayout = emptyPipelineLayout;
    auto renderShaders   = Resource::GetShaderProgram(ParticleRender);
    return LoadAndCreateShaders(
               {.path = Resource::Paths::ParticleRenderVS, .fallback = renderShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::ParticleRenderPS, .fallback = renderShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT}) // <-- FIXED: Changed from R16G16B16_SFLOAT
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .AdditiveBlend()
                .AlphaBlend()
                .CullNone()
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) { particleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

std::expected<void, Error> RenderContext::Impl::BuildMeshParticlePipelines() {
    using enum Resource::ShaderID;

    // 1. Compute Simulation Pipeline (mesh_particle_update.hlsl)
    //    VK_EXT_descriptor_heap: heap mappings + vkCmdPushDataEXT replace the
    //    descriptor set + push constant range this pipeline used to declare.
    auto csMeshShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(MeshParticleUpdate).vertex);

    if (!meshParticleUpdatePass.BuildHeap(ctx.Device(), csMeshShader, &sceneHeapMappings.info)) {
        ZHLN::Log("ERROR: Failed to build 3D mesh particle update compute pipeline!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // 2. All 3D mesh particle graphics pipelines are descriptor-heap pipelines
    //    sharing the empty layout + the scene registry mappings.
    meshParticleRenderLayout = emptyPipelineLayout;

    // 3. G-Buffer Deferred Graphics Pipeline (mesh_particle_render.hlsl)
    auto mpRenderShaders = Resource::GetShaderProgram(MeshParticleRender);
    return LoadAndCreateShaders(
               {.path = Resource::Paths::MeshParticleRenderVS, .fallback = mpRenderShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::MeshParticleRenderPS, .fallback = mpRenderShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array) // Writes to SceneColor, Velocity, NormRough
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true) // Solid 3D geometry writes depth
                .CullBack()
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) { meshParticleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            // 4. Directional Shadow Cascade Pipeline (mesh_particle_shadow.hlsl)
            auto mpShadowShaders = Resource::GetShaderProgram(MeshParticleShadow);
            return LoadAndCreateShaders(
                       {.path = Resource::Paths::MeshParticleShadowVS, .fallback = mpShadowShaders.vertex, .entryPoint = "VSMain"},
                       {.path = Resource::Paths::MeshParticleShadowPS, .fallback = mpShadowShaders.fragment, .entryPoint = "PSShadow"}
            )
                .and_then([&](auto&& shaders) -> std::expected<void, Error> {
                    return Vk::PipelineBuilder<0, true> {}
                        .Shaders(shaders)
                        .Layout(emptyPipelineLayout)
                        .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                        .DepthOnly()
                        .DepthFormat(VK_FORMAT_D32_SFLOAT)
                        .CullNone()
                        .Build(ctx.Device())
                        .transform([&](auto&& pipeline) { meshParticleShadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
                });
        });
}

std::expected<void, Error> RenderContext::Impl::InitSubsystems(const RenderConfig& cfg, int width, int height) {
    using enum Resource::ShaderID;

    return allocator.Init(ctx)
        .and_then([&]() {
            return stagingRingBuffer.Init(
                allocator.Get(), ctx.Device(), ctx.GraphicsQueue(), ctx.PhysicalInfo().graphics_family, static_cast<VkDeviceSize>(64 * 1024 * 1024)
            );
        })
        .and_then([&]() {
            return transferRingBuffer.Init(
                allocator.Get(), ctx.Device(), ctx.TransferQueue(), ctx.PhysicalInfo().transfer_family, static_cast<VkDeviceSize>(64 * 1024 * 1024)
            );
        })
        .and_then([&]() {
            bool supportsRayTracing = CheckRayTracingSupport(ctx.Physical());
            if (!supportsRayTracing || !rtCtx.Init(ctx.Device())) {
                ZHLN::Log("WARNING: Raytracing context failed to initialize. RTR will be disabled.");
            } else {
                ZHLN::Log("Raytracing context initialized successfully.");
            }

            gpuProfiler.Init(ctx.Device(), ctx.Physical(), ctx.PhysicalInfo().graphics_family);
            graphicsCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().graphics_family);
            transferCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().transfer_family);
            computeCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().compute_family);

            return InitShadowResources();
        })
        // VK_EXT_descriptor_heap ordering: InitBindless must run FIRST. It
        // initializes the heaps and reserves the globalTextures[] region, and
        // every later pass binding allocates its slots AFTER that region.
        // Allocating pass slots first (the old order) let culling/cluster
        // descriptors land inside the texture array and clobber it.
        .and_then([&]() { return InitBindless(); })
        .and_then([&]() { return InitCullingResources(); })
        .and_then([&]() { return InitLineBuffers(); })
        .and_then([&]() { return BuildLinePipeline(); })
        .and_then([&]() { return BuildHangGpuPipeline(); })
        .and_then([&]() { return BuildHiZPipeline(); })
        .and_then([&]() { return BuildProceduralBakePipeline(); })
        .and_then([&]() {
            // Shadow variant: geometry + fragment stages from one lookup so
            // their varying locations cannot drift apart.
            const auto shadowShaders = Resource::GetSceneShaders(Resource::SceneShaderVariant::Shadow);
            return CompileShadowPipeline(ctx.Device(), Resource::ShaderPair {.vertex = shadowShaders.vertex, .fragment = shadowShaders.fragment})
                .transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&]() {
            return CompilePunctualShadowPipeline(ctx.Device(), Resource::GetShaderProgram(PunctualShadows)).transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&]() { return InitCSGPipelines(); })
        .and_then([&]() { return presentation.Init(ctx, allocator, surface.Get(), width, height, cfg.vsync); })
        .and_then([&]() {
            sync  = Vk::FrameSync<2>::Create(ctx.Device());
            pools = Vk::CommandPools<2>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1});
            computePools =
                Vk::CommandPools<2, Vk::QueueType::Compute>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().compute_family, .buffersPerPool = 1});
            return InitPostProcessing();
        })
        .and_then([&]() -> std::expected<void, Error> {
            auto* windowHandle = window.IsTTY() ? nullptr : static_cast<GLFWwindow*>(window.GetNativeHandle());
            return SetupUI(windowHandle);
        })
        .and_then([&]() { return InitUIDynamicBuffers(); })
        .and_then([&]() -> std::expected<void, Error> {
            uint32_t workerCount = TaskSystem::GetWorkerCount() + 1;
            if (workerCount == 0) {
                workerCount = 1;
            }
            workerCmds.resize(workerCount);

            for (auto& worker: workerCmds) {
                for (auto& pool: worker.pools) {
                    pool     = Vk::CommandPool<Vk::QueueType::Graphics>(ctx.Device(), ctx.PhysicalInfo().graphics_family);
                    auto res = pool.AllocateSecondary(256);
                    if (!res) [[unlikely]] {
                        return std::unexpected(res.error()); // Implicitly maps VkResult -> Error
                    }
                }
            }
            return {};
        })
        .and_then([&]() {
            return parallelRecorder[0].Init(ctx.Device(), ctx.PhysicalInfo().graphics_family).transform_error([](auto) -> Error {
                return RenderInitError::ParallelRecorderInitializationFailed;
            });
        })
        .and_then([&]() {
            return parallelRecorder[1].Init(ctx.Device(), ctx.PhysicalInfo().graphics_family).transform_error([](auto) -> Error {
                return RenderInitError::ParallelRecorderInitializationFailed;
            });
        })
        .transform([&]() {
            deletionQueue.Init(2);
            auto fvb_res = CreateDoubleBuffered(
                allocator, sizeof(GPUVolumetricVolume) * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU
            );
            if (fvb_res) {
                frames.fogVolumesBuffer = std::move(*fvb_res);
            }
        });
}

namespace {

std::expected<Vk::ExtensionResult, Error> GetPlatformInstanceExtensions(Window& window) noexcept {
    auto builder = Vk::ExtensionBuilder::ForInstance();

    if (window.IsHeadless()) {
        // True headless mode: no surface extensions required. GLFW is not
        // initialised, so we must not call any GLFW functions here.
    } else if (window.IsTTY()) {
        for (const auto ext: TTYBackend::GetRequiredInstanceExtensions()) {
            builder.Require(ext);
        }
    } else {
        glfwSetErrorCallback([](int error, const char* description) { ZHLN::Log("[GLFW Error] Code {}: {}", error, description); });

        uint32_t     glfwExtensionCount = 0;
        const char** glfwExtensions     = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        if (glfwExtensionCount > 0 && glfwExtensions != nullptr) {
            for (uint32_t i = 0; i < glfwExtensionCount; ++i) {
                builder.Require(glfwExtensions[i]);
            }
        } else {
            ZHLN::Log("WARNING: glfwGetRequiredInstanceExtensions returned 0 extensions.");
            builder.Require(VK_KHR_SURFACE_EXTENSION_NAME).Optional("VK_KHR_wayland_surface").Optional("VK_KHR_xcb_surface").Optional("VK_KHR_xlib_surface");
        }

        builder.Require(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME).Require(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }

    return std::move(builder)
        .Debug(true) // Render-graph checkpoints use VK_EXT_debug_utils when available.
        .OptionalIf("VK_KHR_portability_enumeration", isMac)
        .Build()
        .transform_error([](auto err) -> Error { return err; });
}

auto BuildFeatureChain(VkPhysicalDevice physicalDevice, const HardwareCaps& caps, ValidationMode validationMode) noexcept {
    return Vk::FeatureChainBuilder(physicalDevice)
        .Optional<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>([](auto& f) { f.swapchainMaintenance1 = VK_TRUE; })
        .Require<VkPhysicalDeviceVulkan11Features>([](auto& f) {
            f.multiview                          = VK_TRUE;
            f.storageBuffer16BitAccess           = VK_TRUE;
            f.uniformAndStorageBuffer16BitAccess = VK_TRUE;
            f.shaderDrawParameters               = VK_TRUE;
        })
        .Require<VkPhysicalDeviceVulkan13Features>([](auto& f) {
            f.synchronization2               = VK_TRUE;
            f.dynamicRendering               = VK_TRUE;
            f.shaderDemoteToHelperInvocation = VK_TRUE;
        })
        .Require<VkPhysicalDeviceVulkan12Features>([&](auto& f) {
            f.descriptorIndexing                           = VK_TRUE;
            f.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
            f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            f.descriptorBindingPartiallyBound              = VK_TRUE;
            f.runtimeDescriptorArray                       = VK_TRUE;
            f.bufferDeviceAddress                          = VK_TRUE;
            f.hostQueryReset                               = VK_TRUE;
            f.timelineSemaphore                            = VK_TRUE;
            f.drawIndirectCount                            = caps.supportsDrawIndirectCount ? VK_TRUE : VK_FALSE;
            f.uniformAndStorageBuffer8BitAccess            = VK_TRUE;
            f.shaderFloat16                                = VK_TRUE;

            if (validationMode == ZHLN::ValidationMode::GPU) {
                f.scalarBlockLayout            = VK_TRUE;
                f.storageBuffer8BitAccess      = VK_TRUE;
                f.shaderInt8                   = VK_TRUE;
                f.vulkanMemoryModel            = VK_TRUE;
                f.vulkanMemoryModelDeviceScope = VK_TRUE;
            }
        })
        .Optional<VkPhysicalDeviceAccelerationStructureFeaturesKHR>([](auto& f) { f.accelerationStructure = VK_TRUE; })
        .Optional<VkPhysicalDeviceRayQueryFeaturesKHR>([](auto& f) { f.rayQuery = VK_TRUE; })
        .Optional<VkPhysicalDeviceRobustness2FeaturesEXT>([validationMode](auto& f) {
            f.nullDescriptor = VK_TRUE;

            if (validationMode == ZHLN::ValidationMode::GPU) {
                f.robustBufferAccess2 = VK_TRUE;
                f.robustImageAccess2  = VK_TRUE;
            }
        })
        // VK_EXT_descriptor_heap: the whole scene binding model now lives in
        // descriptor heaps; the legacy set path remains only for passes that
        // have not been ported yet (post-processing, volumetric, ImGui, ...).
        .Require<VkPhysicalDeviceDescriptorHeapFeaturesEXT>([](auto& f) { f.descriptorHeap = VK_TRUE; })
        // Pipelines declare a stencil attachment format derived from the depth
        // format, but only some passes actually bind stencil; this feature lets
        // them draw inside stencil-less render passes (and stencil-less
        // secondary command buffers) without format-mismatch VUIDs.
        .Require<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>([](auto& f) { f.dynamicRenderingUnusedAttachments = VK_TRUE; })
        // VK_EXT_mesh_shader. multiviewMeshShader lets the shadow pass render
        // all cascades from a single dispatch; it is only requested when the
        // device actually supports mesh shading, because the feature struct
        // must not be chained on a device that lacks the extension.
        .Optional<VkPhysicalDeviceMeshShaderFeaturesEXT>([&caps](auto& f) {
            f.taskShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            f.meshShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            // Only requested when the device actually has it: one unsupported
            // bit would make FeatureChain::Optional discard the entire struct,
            // leaving the extension enabled but task/mesh shading OFF.
            f.multiviewMeshShader = caps.supportsMultiviewMeshShader ? VK_TRUE : VK_FALSE;
        })
        .Require<VkPhysicalDeviceFeatures2>([&](auto& f) {
            f.features.multiDrawIndirect         = VK_TRUE;
            f.features.samplerAnisotropy         = VK_TRUE;
            f.features.drawIndirectFirstInstance = VK_TRUE;
            f.features.shaderInt64               = caps.supportsInt64 ? VK_TRUE : VK_FALSE;
            f.features.imageCubeArray            = VK_TRUE;
            f.features.shaderInt16               = VK_TRUE;

            if (validationMode == ZHLN::ValidationMode::GPU) {
                f.features.robustBufferAccess             = VK_TRUE;
                f.features.fragmentStoresAndAtomics       = VK_TRUE;
                f.features.vertexPipelineStoresAndAtomics = VK_TRUE;
                f.features.shaderInt16                    = VK_TRUE;
            }
        })
        .Build();
}

std::expected<Vk::ExtensionResult, Error> GetDeviceExtensions(VkPhysicalDevice physicalDevice, bool isHeadless, bool meshShaderSupported) noexcept {
    auto builder = Vk::ExtensionBuilder::ForDevice(physicalDevice);

    if (!isHeadless) {
        builder.Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            .Optional(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)
            .Optional(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
    }

    return builder.Optional("VK_EXT_robustness2")
        .OptionalIf("VK_KHR_portability_subset", isMac)
        .OptionalGroup(
            {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
            CheckRayTracingSupport(physicalDevice)
        )
        // VK_EXT_descriptor_heap replaces descriptor sets/pools/layouts for the
        // scene path. VK_KHR_maintenance5 (or Vulkan 1.4) provides
        // VkPipelineCreateFlags2CreateInfoKHR for the mandatory
        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT pipeline flag.
        // VK_EXT_extended_dynamic_state3 provides dynamicRenderingUnusedAttachments:
        // without it, the material pipelines' stencilAttachmentFormat
        // (D32_SFLOAT_S8_UINT) cannot legally be drawn inside the stencil-less
        // MainPass1 secondary command buffers (VUID-...-08917/06775).
        .Require(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME)
        .Require(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)
        .Require(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)
        // VK_EXT_mesh_shader replaces the input assembler + vertex stage of the
        // geometry passes with task/mesh shaders. It stays OPTIONAL: the vertex
        // pipeline is still built for every material, so devices without mesh
        // shading (or with limits below our meshlet budget) keep rendering.
        // Support was already probed once into HardwareCaps; re-probing here
        // would repeat the diagnostics for every failure.
        .OptionalGroup({VK_EXT_MESH_SHADER_EXTENSION_NAME}, meshShaderSupported)
        .Build()
        .transform_error([](auto err) -> Error { return err; });
}

template <typename LayoutT>
[[nodiscard]] std::expected<void, Error> BuildPassHelper(
    RenderContext::Impl*            self,
    Vk::PostProcessPass<LayoutT>&   pass,
    const char*                     passName,
    VertexStageSource               vs,
    FragmentStageSource             ps,
    std::initializer_list<VkFormat> colorFormats,
    bool                            additive = false
) noexcept {
    return self->LoadAndCreateShaders(vs, ps).and_then([&](auto&& shaders) -> std::expected<void, Error> {
        // VK_EXT_descriptor_heap: the pass is a heap pipeline (null layout,
        // PUSH_INDEX mapping table baked from the reflected set layout). Per-
        // draw data travels through push data, so no push ranges are declared.
        if (pass.BuildHeap(self->ctx.Device(), self->heapManager, shaders, colorFormats, additive)) {
            ZHLN::Log("[RenderInit] Successfully built pipeline for pass: {}", passName);
            return {};
        }
        ZHLN::Log("[RenderInit] ERROR: Failed to build pipeline for pass: {}", passName);
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    });
}

template <typename LayoutT>
[[nodiscard]] std::expected<void, Error> BuildPassVariants(
    RenderContext::Impl*                  self,
    Vk::PostProcessPass<LayoutT>&         pass,
    const char*                           passName,
    VertexStageSource                     vs,
    FragmentStageSource                   ps,
    std::initializer_list<VkFormat>       colorFormats,
    std::span<const VkSpecializationInfo> specInfos,
    bool                                  additive = false
) noexcept {
    return self->LoadAndCreateShaders(vs, ps).and_then([&](auto&& shaders) -> std::expected<void, Error> {
        // VK_EXT_descriptor_heap: specialization never changes the descriptor
        // interface, so one mapping table covers every variant.
        if (pass.BuildHeapVariants(self->ctx.Device(), self->heapManager, shaders, colorFormats, specInfos, additive)) {
            ZHLN::Log("[RenderInit] Successfully built pipeline variants for pass: {}", passName);
            return {};
        }
        ZHLN::Log("[RenderInit] ERROR: Failed to build pipeline variants for pass: {}", passName);
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    });
}

} // namespace

std::expected<Vk::ShaderStages, Error> RenderContext::Impl::LoadAndCreateShaders(VertexStageSource vs, FragmentStageSource ps) const noexcept {
    const void*           vs_code = nullptr;
    size_t                vs_size = 0;
    const void*           ps_code = nullptr;
    size_t                ps_size = 0;
    std::vector<uint32_t> disk_vs;
    std::vector<uint32_t> disk_ps;

    LoadShaderData(vs, vs_code, vs_size, disk_vs);
    LoadShaderData(ps, ps_code, ps_size, disk_ps);

    gpuDiagnostics.RegisterShader({.code = Vk::AsSpirV(vs_code), .size = vs_size, .entry_point = vs.entryPoint}, "VSMain");
    gpuDiagnostics.RegisterShader({.code = Vk::AsSpirV(ps_code), .size = ps_size, .entry_point = ps.entryPoint}, "PSMain");

    return Vk::ShaderStages::Create(
               ctx.Device(), {.code = Vk::AsSpirV(vs_code), .size = vs_size, .entry_point = vs.entryPoint},
               {.code = Vk::AsSpirV(ps_code), .size = ps_size, .entry_point = ps.entryPoint}
    )
        .transform_error([](auto err) -> Error { return err; });
}

std::expected<Vk::Pipeline, Error> RenderContext::Impl::LoadAndCreateComputeShader(ComputeStageSource cs, VkPipelineLayout layout) const noexcept {
    const void*           cs_code = nullptr;
    size_t                cs_size = 0;
    std::vector<uint32_t> disk_cs;

    LoadShaderData(cs, cs_code, cs_size, disk_cs);

    gpuDiagnostics.RegisterShader({.code = Vk::AsSpirV(cs_code), .size = cs_size, .entry_point = cs.entryPoint}, "CSMain");

    return Vk::ComputePipelineBuilder()
        .Shader(Vk::AsSpirV(cs_code), cs_size, cs.entryPoint)
        .Layout(layout)
        .Build(ctx.Device())
        .transform_error([](ZHLN::Error err) -> Error {
            if (err.Is(Vk::PipelineBuilderResult::MissingShaders)) {
                return RenderInitError::ShaderCompilationFailed;
            }
            if (err.Is(Vk::PipelineBuilderResult::MissingLayout)) {
                return RenderInitError::PipelineLayoutCreationFailed;
            }
            return RenderInitError::PipelineCreationFailed;
        });
}

void RenderContext::Impl::WatchPipeline(const char* vsPath, const char* psPath, std::function<void()> rebuild_fn) noexcept {
    if constexpr (isDev) {
        RegisterShaderWatcher(vsPath, rebuild_fn);
        RegisterShaderWatcher(psPath, std::move(rebuild_fn));
    }
}

RenderContext::RenderContext(PrivateToken /*unused*/, std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized" // Suppress false-positives inside std::expected::transform
#endif

std::expected<std::unique_ptr<RenderContext>, Error> RenderContext::Create(Window& window, const RenderConfig& cfg) noexcept {
    auto impl     = std::make_unique<Impl>(window);
    impl->appName = cfg.appName;

    VkInstance              instance    = VK_NULL_HANDLE;
    VkSurfaceKHR            raw_surface = VK_NULL_HANDLE;
    int                     width       = 0;
    int                     height      = 0;
    ZHLN_PhysicalDeviceInfo physicalInfo {};

    return GetPlatformInstanceExtensions(window)
        .and_then([&](auto&& inst_exts) -> std::expected<void, Error> {
            return Vk::Context::Builder()
                .AppName(impl->appName)
                .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                .InstanceExtensions(inst_exts)
                .BuildInstance()
                .transform([&](VkInstance inst) { instance = inst; });
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (!window.IsTTY() && !window.IsHeadless()) {
                return window.CreateVulkanSurface(instance, nullptr, width, height)
                    .transform_error([](auto) -> Error { return RenderInitError::SurfaceCreationFailed; })
                    .transform([&](void* surface) { raw_surface = static_cast<VkSurfaceKHR>(surface); });
            }
            if (window.IsHeadless()) {
                // Headless: obtain offscreen dimensions without creating a VkSurfaceKHR
                return window.CreateVulkanSurface(instance, nullptr, width, height)
                    .transform_error([](auto) -> Error { return RenderInitError::SurfaceCreationFailed; })
                    .transform([&](void* /*surface*/) { raw_surface = VK_NULL_HANDLE; });
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            return Vk::Context::Builder().Instance(instance).Surface(raw_surface).SelectPhysicalDevice().transform([&](const ZHLN_PhysicalDeviceInfo& info) {
                physicalInfo = info;
            });
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (window.IsTTY()) {
                return window.CreateVulkanSurface(instance, physicalInfo.handle, width, height)
                    .transform_error([](auto) -> Error { return RenderInitError::SurfaceCreationFailed; })
                    .transform([&](void* surface) { raw_surface = static_cast<VkSurfaceKHR>(surface); });
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            impl->surface         = Vk::Surface(instance, raw_surface);
            HardwareCaps caps     = ProbeHardware(physicalInfo.handle, physicalInfo.properties.properties.apiVersion);
            auto         features = BuildFeatureChain(physicalInfo.handle, caps, cfg.validationMode);

            return GetDeviceExtensions(physicalInfo.handle, window.IsHeadless(), caps.supportsMeshShader)
                .and_then([&](auto&& dev_exts) -> std::expected<void, Error> {
                    std::vector<const char*> devExtList = dev_exts;

                    return Vk::Context::Builder()
                        .Instance(instance)
                        .Surface(raw_surface)
                        .PhysicalDevice(physicalInfo)
                        .DeviceExtensions(devExtList)
                        .DeviceFeatures(features.GetRoot())
                        .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                        .Build()
                        .transform([&](auto&& context) {
                            impl->ctx         = std::forward<decltype(context)>(context);
                            const auto vendor = static_cast<Vk::GPUVendor>(physicalInfo.properties.properties.vendorID);
                            impl->gpuDiagnostics.Create(vendor, impl->ctx.Device(), impl->ctx.Physical());
                        });
                });
        })
        .and_then([&]() { return impl->InitSubsystems(cfg, width, height); })
        .transform([&]() { return std::make_unique<RenderContext>(PrivateToken {}, std::move(impl)); });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

std::expected<void, Error> RenderContext::Impl::BuildSkinningPipeline() {
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .AddPushConstant(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(SkinningConstants))
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            skinningPass.pipelineLayout = std::forward<decltype(layout)>(layout);
            return LoadAndCreateComputeShader({.path = Resource::Paths::SkinningCS, .fallback = Resource::skinning_comp}, skinningPass.pipelineLayout.Get())
                .transform([&](auto&& pipeline) { skinningPass.pipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

RenderContext::~RenderContext() {
    if (_impl && (_impl->ctx.Device() != nullptr)) {
        _impl->gpuDiagnostics.Shutdown();
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (res != VK_SUCCESS) {
            ZHLN::Log("ERROR: Failed to wait for idle on device destruction.");
        }
        _impl->stagingContext.reset();

        // --- SAFETY: Only shut down ImGui if it was actually initialized ---
        if (!_impl->window.IsTTY() && !_impl->window.IsHeadless()) {
            ImGui_ImplVulkanHeap_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
    }
}

std::expected<void, Error> RenderContext::Impl::AllocateDynamicVertexBuffers(
    size_t                           maxVertices,
    DoubleBuffered<Vk::Buffer>&      bufs,
    DoubleBuffered<VkDeviceAddress>& addrs,
    VkBufferUsageFlags               extraFlags,
    const char*                      label
) noexcept {
    const size_t bufferSize = maxVertices * (sizeof(VertexPosition) + sizeof(VertexAttributes));

    for (int i = 0; i < 2; ++i) {
        auto res = Vk::Buffer::Create(
            allocator.Get(), bufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extraFlags,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        if (!res) {
            return std::unexpected(res.error());
        }
        bufs[i]  = std::move(*res);
        addrs[i] = ctx.BufferAddress(bufs[i].Handle());
    }
    ZHLN::Log("Allocated double-buffered dynamic {} VBOs ({} bytes).", label, bufferSize);
    return {};
}

std::expected<void, Error> RenderContext::Impl::InitLineBuffers() noexcept {
    return AllocateDynamicVertexBuffers(kMaxLineVertices, frames.lineVbos, frames.lineVboAddresses, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "line");
}

std::expected<void, Error> RenderContext::Impl::BuildLinePipeline() {
    linePipelineLayout = emptyPipelineLayout;

    // The debug line pipeline rasterises through PSForward, so it needs the
    // Forward geometry variant (the G-buffer one emits motion vectors and a
    // normal frame that PSForward does not read).
    const auto forwardShaders = Resource::GetSceneShaders(Resource::SceneShaderVariant::Forward);

    return LoadAndCreateShaders(
               {.path = Resource::Paths::BasicVSForward, .fallback = forwardShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::ForwardPS, .fallback = forwardShaders.fragment, .entryPoint = "PSForward"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineBuilder<1, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                .CullNone()
                .AlphaBlend()
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) { linePipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

std::expected<void, Error> RenderContext::Impl::InitShadowResources() {
    using enum RenderInitError;

    auto shadowSamplerBuilder = Vk::SamplerBuilder {}.Linear().ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE).DepthCompare();

    return shadowSamplerBuilder.Build(ctx.Device())
        .transform_error([](auto err) -> Error { return err; })

        // 1. Bind Sampler
        .and_then([&](auto&& sampler) -> std::expected<void, Error> {
            shadowSampler     = std::forward<decltype(sampler)>(sampler);
            shadowSamplerInfo = shadowSamplerBuilder.Info();
            return {};
        })

        // 2. Allocate Cascaded Shadow Map Render Target
        .and_then([&]() -> std::expected<void, Error> {
            auto sm_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = NUM_CASCADES}
            );
            if (!sm_res) {
                return std::unexpected(SubsystemAllocationFailed);
            }
            graphResources.shadowMap = std::move(*sm_res);

            auto smp_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = NUM_CASCADES}
            );
            if (!smp_res) {
                return std::unexpected(SubsystemAllocationFailed);
            }
            shadowMapPrev = std::move(*smp_res);
            return {};
        })

        // 3. Create Cascade Image Views
        .and_then([&]() -> std::expected<void, Error> {
            shadowCascadeViews.resize(NUM_CASCADES);
            shadowCascadeViewsPrev.resize(NUM_CASCADES);
            for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
                {
                    auto view_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowMap.image.Handle(), i, 1);
                    if (!view_res) {
                        return std::unexpected(SubsystemAllocationFailed);
                    }
                    shadowCascadeViews[i] = std::move(*view_res);
                }
                {
                    auto prev_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), shadowMapPrev.image.Handle(), i, 1);
                    if (!prev_res) {
                        return std::unexpected(SubsystemAllocationFailed);
                    }
                    shadowCascadeViewsPrev[i] = std::move(*prev_res);
                }
                if (!shadowCascadeViews[i].Valid() || !shadowCascadeViewsPrev[i].Valid()) [[unlikely]] {
                    return std::unexpected(SubsystemAllocationFailed);
                }
            }
            return {};
        })

        // 4. Allocate Punctual Shadow Atlas Render Target
        .and_then([&]() -> std::expected<void, Error> {
            auto sa_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = 1024, .height = 1024},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = 24}
            );
            if (!sa_res) [[unlikely]] {
                return std::unexpected(SubsystemAllocationFailed);
            }
            graphResources.shadowAtlas = std::move(*sa_res);
            return {};
        })

        // 5. Create Atlas Image Views
        .and_then([&]() -> std::expected<void, Error> {
            {
                auto cube_res = Vk::CreateViewCubeArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 24);
                if (!cube_res) {
                    return std::unexpected(SubsystemAllocationFailed);
                }
                shadowAtlasCubeView = std::move(*cube_res);
            }
            {
                auto array_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 0, 24);
                if (!array_res) {
                    return std::unexpected(SubsystemAllocationFailed);
                }
                shadowAtlas2DView = std::move(*array_res);
            }
            shadowAtlasCubeViewInfo = {
                .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext      = nullptr,
                .flags      = 0,
                .image      = graphResources.shadowAtlas.image.Handle(),
                .viewType   = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
                .format     = VK_FORMAT_D32_SFLOAT,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 24},
            };
            shadowAtlas2DViewInfo = {
                .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext      = nullptr,
                .flags      = 0,
                .image      = graphResources.shadowAtlas.image.Handle(),
                .viewType   = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format     = VK_FORMAT_D32_SFLOAT,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 24},
            };
            if (!shadowAtlasCubeView.Valid() || !shadowAtlas2DView.Valid()) [[unlikely]] {
                return std::unexpected(SubsystemAllocationFailed);
            }
            return {};
        })

        // 6. Transition Layouts and Recreate Punctual Views
        .and_then([&]() -> std::expected<void, Error> {
            Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, shadowMapPrev.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, shadowMapPrev.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
            });

            RecreatePunctualShadowViews();
            return {};
        })

        // 7. Allocate Double-Buffered Frame Uniform Buffers
        //    VK_EXT_descriptor_heap: their device addresses feed the scene
        //    registry's PUSH_ADDRESS mappings, so they need
        //    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
        .and_then([&]() {
            return CreateDoubleBuffered(
                       allocator, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VMA_MEMORY_USAGE_CPU_TO_GPU
            )
                .transform_error([](auto err) -> Error { return err; });
        })

        // 8. Allocate Double-Buffered Light Storage Buffers (same SDA requirement)
        .and_then([&](auto&& fub) {
            frames.frameUniformBuffers = std::forward<decltype(fub)>(fub);
            return CreateDoubleBuffered(
                       allocator, sizeof(GPULight) * 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VMA_MEMORY_USAGE_CPU_TO_GPU
            )
                .transform_error([](auto err) -> Error { return err; });
        })

        // 9. Allocate Double-Buffered Indirect Argument Buffers
        .and_then([&](auto&& lsb) {
            frames.lightStorageBuffers = std::forward<decltype(lsb)>(lsb);
            return CreateDoubleBuffered(
                       allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances * 8, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
            )
                .transform_error([](auto err) -> Error { return err; });
        })

        // 10. Complete pipeline assignment
        .transform([&](auto&& sib) { frames.shadowIndirectBuffers = std::forward<decltype(sib)>(sib); });
}

std::expected<void, Error> RenderContext::Impl::InitCullingResources() {
    using enum Resource::ShaderID;

    // 1. Reflect the culling layout (drives the heap mapping table), then
    //    build a heap pipeline. The push index selects {pass 0/1} x {frame
    //    parity} slot spans, so one pipeline serves both culling passes and
    //    both frames in flight.
    auto cullingShader = Vk::CreateShaderDesc(Resource::culling_comp);
    if (!cullingLayout.Build(ctx.Device(), cullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        ZHLN::Log("[RenderInit] ERROR: Failed to reflect culling layout from culling SPV!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    Vk::BuildHeapPassBindings(heapManager, cullingLayout.reflectedSets[0], 0, Vk::kHeapIndexPushOffset, 4, cullingHeapBindings);

    auto make_instance_set = [&](uint32_t i) -> std::expected<void, Error> {
        return Vk::Buffer::Create(
                   allocator.Get(), sizeof(InstanceData) * kGpuCullingMaxInstances,
                   // VK_EXT_descriptor_heap: instance-data buffer address feeds
                   // the scene registry's PUSH_ADDRESS mapping.
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
        )
            .and_then([&, i](auto&& idb) {
                frames.instanceDataBuffers[i] = std::forward<decltype(idb)>(idb);
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances,
                    // VK_EXT_descriptor_heap: bound through heap buffer
                    // descriptors, so the buffer needs a device address.
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .and_then([&, i](auto&& icb1) {
                frames.indirectCommandsBuffers[i] = std::forward<decltype(icb1)>(icb1);
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .and_then([&, i](auto&& icb2) {
                frames.indirectCommandsBuffersPass2[i] = std::forward<decltype(icb2)>(icb2);
                // NOTE: TRANSFER_SRC on both indirect buffers exists for the
                // ZHLN_DEBUG_INDIRECT readback (end-of-frame head copy).
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(uint32_t) * kGpuCullingMaxInstances,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .and_then([&, i](auto&& spcb) {
                frames.secondPassCandidatesBuffers[i] = std::forward<decltype(spcb)>(spcb);
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(uint32_t),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .transform([&, i](auto&& spcnt) -> void {
                // This buffer is the second-pass candidate counter, written by
                // pass-1 culling and consumed by pass-2.  It is intentionally
                // NOT routed through globalCounterBuffers (which serves the
                // cluster-culling global counter allocated later in
                // make_cluster_set) to avoid a moved-from ordering hazard.
                frames.secondPassCountBuffers[i] = std::forward<decltype(spcnt)>(spcnt);
            });
    };

    return make_instance_set(0)
        .and_then([&]() { return make_instance_set(1); })
        .and_then([&]() { return cullingPass.BuildHeap(ctx.Device(), cullingShader, cullingHeapBindings.GetInfo()); })
        .and_then([&]() -> std::expected<void, Error> {
            constexpr auto numClusters = static_cast<size_t>(16 * 9 * 24);

            return Vk::Buffer::Create(
                       allocator.Get(), sizeof(struct ClusterBounds) * numClusters,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY
            )
                .transform_error([](VkResult res) -> Error { return res; })
                .and_then([&, numClusters](auto&& cbb) -> std::expected<void, Error> {
                    clusterBoundsBuffer = std::forward<decltype(cbb)>(cbb);

                    // VK_EXT_descriptor_heap: reflect the set layout and bake the
                    // binding table (frame-parity slot spans).
                    auto ccShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterCulling).vertex);
                    if (!clusterCullingDescLayout.Build(ctx.Device(), ccShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
                        ZHLN::Log("[RenderInit] ERROR: Failed to reflect cluster-culling layout!");
                        return std::unexpected(RenderInitError::PipelineCreationFailed);
                    }
                    Vk::BuildHeapPassBindings(
                        heapManager, clusterCullingDescLayout.reflectedSets[0], 0, Vk::kHeapIndexPushOffset, 2, clusterCullingHeapBindings
                    );

                    auto make_cluster_set = [&](uint32_t i) -> std::expected<void, Error> {
                        return Vk::Buffer::Create(
                                   allocator.Get(), sizeof(ClusterVolume) * numClusters,
                                   // VK_EXT_descriptor_heap: heap buffer descriptors need device addresses.
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VMA_MEMORY_USAGE_GPU_ONLY
                        )
                            .transform_error([](VkResult res) -> Error { return res; })
                            .and_then([&, i](auto&& cgb) {
                                frames.clusterGridBuffers[i] = std::forward<decltype(cgb)>(cgb);
                                return Vk::Buffer::Create(
                                           allocator.Get(), sizeof(uint32_t) * numClusters * 64,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                                )
                                    .transform_error([](VkResult res) -> Error { return res; });
                            })
                            .and_then([&, i](auto&& lsb) {
                                frames.lightIndexListBuffers[i] = std::forward<decltype(lsb)>(lsb);
                                return Vk::Buffer::Create(
                                           allocator.Get(), sizeof(uint32_t),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                           VMA_MEMORY_USAGE_GPU_ONLY
                                )
                                    .transform_error([](VkResult res) -> Error { return res; });
                            })
                            .transform([&, i](auto&& gcb) -> void {
                                frames.globalCounterBuffers[i] = std::forward<decltype(gcb)>(gcb);

                                // Zero-initialize clusterGridBuffer and globalCounterBuffer to guarantee zero counts
                                Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
                                    Vk::FillBuffer(cmd, frames.clusterGridBuffers[i], 0, 0u);
                                    Vk::FillBuffer(cmd, frames.globalCounterBuffers[i], 0, 0u);
                                });

                                // VK_EXT_descriptor_heap: write the cluster-culling
                                // descriptors into the parity slot spans. Order
                                // mirrors cluster_culling.slang's set-0 declaration
                                // order: in_Bounds, out_Grid, out_IndexList,
                                // out_Counter, frame, lights.
                                Vk::WriteHeapBindings(
                                    heapManager, ctx, clusterCullingHeapBindings, i, clusterBoundsBuffer, frames.clusterGridBuffers[i],
                                    frames.lightIndexListBuffers[i], frames.globalCounterBuffers[i], frames.frameUniformBuffers[i],
                                    frames.lightStorageBuffers[i]
                                );
                            });
                    };

                    return make_cluster_set(0).and_then([&, make_cluster_set]() { return make_cluster_set(1); });
                });
        })
        .and_then([&]() -> std::expected<void, Error> {
            // The cluster-bounds pass only touches {out_Bounds, frame}, so it gets
            // its own reflected layout rather than sharing clusterCullingDescLayout.
            auto bDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterBounds).vertex);
            if (!clusterBoundsDescLayout.Build(ctx.Device(), bDesc, VK_SHADER_STAGE_COMPUTE_BIT)) {
                ZHLN::Log("[RenderInit] ERROR: Failed to reflect cluster-bounds layout!");
                return std::unexpected(RenderInitError::PipelineCreationFailed);
            }
            Vk::BuildHeapPassBindings(heapManager, clusterBoundsDescLayout.reflectedSets[0], 0, Vk::kHeapIndexPushOffset, 2, clusterBoundsHeapBindings);
            for (int i = 0; i < 2; ++i) {
                // Order mirrors cluster_bounds.slang's set-0 declaration order: out_Bounds, frame.
                Vk::WriteHeapBindings(heapManager, ctx, clusterBoundsHeapBindings, i, clusterBoundsBuffer, frames.frameUniformBuffers[i]);
            }
            return clusterBoundsPass.BuildHeap(ctx.Device(), bDesc, clusterBoundsHeapBindings.GetInfo());
        })
        .and_then([&]() {
            auto cDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterCulling).vertex);
            return clusterCullingPass.BuildHeap(ctx.Device(), cDesc, clusterCullingHeapBindings.GetInfo());
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (rtCtx.Valid()) {
                ZHLN_AccelerationStructureSizes tlasSizes;
                rtCtx.GetTLASSizes(kGpuCullingMaxInstances, tlasSizes);

                auto make_tlas_set = [&](uint32_t i) -> std::expected<void, Error> {
                    return Vk::Buffer::Create(
                               allocator.Get(), tlasSizes.acceleration_structure_size,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                    )
                        .transform_error([](VkResult res) -> Error { return res; })
                        .and_then([&, i, tlasSizes](auto&& tb) { // Deduced as std::expected<Vk::Buffer, Error>
                            frames.tlasBuffer[i] = std::forward<decltype(tb)>(tb);
                            return Vk::Buffer::Create(
                                       allocator.Get(), tlasSizes.build_scratch_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                            )
                                .transform_error([](VkResult res) -> Error { return res; });
                        })
                        .and_then([&, i, tlasSizes](auto&& tsb) { // Deduced as std::expected<Vk::Buffer, Error>
                            frames.tlasScratchBuffer[i] = std::forward<decltype(tsb)>(tsb);
                            frames.tlas[i] =
                                rtCtx.CreateAccelerationStructure(frames.tlasBuffer[i].Handle(), tlasSizes.acceleration_structure_size, ZHLN_AS_TYPE_TOP_LEVEL);

                            return Vk::Buffer::Create(
                                       allocator.Get(), sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VMA_MEMORY_USAGE_GPU_ONLY
                            )
                                .transform_error([](VkResult res) -> Error { return res; });
                        })
                        .and_then([&, i](auto&& tib) { // Deduced as std::expected<Vk::Buffer, Error>
                            frames.tlasInstanceBuffers[i] = std::forward<decltype(tib)>(tib);
                            return Vk::Buffer::Create(
                                       allocator.Get(), sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VMA_MEMORY_USAGE_CPU_ONLY
                            )
                                .transform_error([](VkResult res) -> Error { return res; });
                        })
                        .transform([&, i](auto&& tstb) -> void { // Transforms final Vk::Buffer to void
                            frames.tlasStagingBuffers[i] = std::forward<decltype(tstb)>(tstb);
                        });
                };

                return make_tlas_set(0).and_then([&, make_tlas_set]() { return make_tlas_set(1); });
            }
            return {};
        })
        .and_then([&]() { return BuildSkinningPipeline(); })
        .transform([&]() {
            if constexpr (isDev) {
                RegisterShaderWatcher(Resource::Paths::SkinningCS, [this]() {
                    auto res = BuildSkinningPipeline();
                    if (!res) {
                        ZHLN::Log("ERROR: Failed to hot-reload Skinning pipeline: {}", res.error().Message());
                    } else {
                        ZHLN::Log("[Shader Reload] Skinning pipeline hot-reloaded successfully.");
                    }
                });
            }
        });
}

std::expected<void, Error> RenderContext::Impl::InitBindless() {
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
                ZHLN::Log("[RenderInit] ERROR: Failed to reflect the global scene registry layout!");
                return std::unexpected(RenderInitError::PipelineCreationFailed);
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
        .and_then([&]() -> std::expected<void, Error> { return InitSkeletalAnimationResources(); })
        .and_then([&]() -> std::expected<void, Error> { return InitLightingLUTs(); })
        .and_then([&]() -> std::expected<void, Error> { return InitializeSystemTextures(); })
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

std::expected<void, Error>
    RenderContext::Impl::InitSceneHeaps(const VkSamplerCreateInfo& globalSamplerInfo, const VkSamplerCreateInfo& clampSamplerInfo) noexcept {
    auto init_res = heapManager.Init(
        ctx, allocator, kSceneStaticResourceSlots + kGlobalTextureSlots + kPassStaticResourceSlots, kSceneDynamicResourceSlots,
        kSceneStaticSamplerSlots + kPassStaticSamplerSlots, kSceneDynamicSamplerSlots, 2
    );
    if (!init_res) {
        ZHLN::Log("[RenderInit] ERROR: Descriptor heap initialization failed (error code {})", static_cast<uint32_t>(init_res.error()));
        return std::unexpected(RenderInitError::SubsystemAllocationFailed);
    }

    // The push-data budget must fit the per-frame device-address block that
    // feeds the scene registry's PUSH_ADDRESS mappings PLUS the per-dispatch
    // descriptor-index word (kHeapIndexPushOffset + 4).
    if (heapManager.PushDataMaxSize() < (Vk::kHeapIndexPushOffset + 4)) [[unlikely]] {
        ZHLN::Log(
            "[RenderInit] ERROR: maxPushDataSize ({}) too small for the push-data layout (needs {})", heapManager.PushDataMaxSize(),
            Vk::kHeapIndexPushOffset + 4
        );
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // --- Static slot allocation (sampler heap) ---
    auto globalSlot = heapManager.AllocateStaticSampler();
    auto clampSlot  = heapManager.AllocateStaticSampler();
    auto pointSlot  = heapManager.AllocateStaticSampler();
    if (!globalSlot || !clampSlot || !pointSlot) {
        ZHLN::Log("[RenderInit] ERROR: Sampler heap slot exhaustion during init.");
        return std::unexpected(RenderInitError::SubsystemAllocationFailed);
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
        ZHLN::Log("[RenderInit] ERROR: Resource heap slot exhaustion during init.");
        return std::unexpected(RenderInitError::SubsystemAllocationFailed);
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
        ZHLN::Log(
            "[RenderInit] ERROR: Heap cursors ({}/{}) drifted before region reservation — pass slots would overlap the texture array",
            heapManager.StaticResourceCursor(), heapManager.StaticSamplerCursor()
        );
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    heapManager.SkipStaticResourceSlots(kGlobalTextureSlots + (kSceneStaticResourceSlots - 4));
    heapManager.SkipStaticSamplerSlots(kSceneStaticSamplerSlots - 3);

    // The ImGui texture region sits at the tail of the pass slot region.
    imguiTextureHeapBase = kPassResourceHeapBase + kPassStaticResourceSlots - kImGuiTextureSlots;

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
    // Per-frame buffers (1..6) use VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT:
    // the push-data block at kHeapFrameAddrPushOffset carries their current
    // device addresses, selected per frame. Images and samplers sit in static
    // heap slots via VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT.
    const auto add_scene_set = [&](uint32_t setIndex, HeapMappingSet& out) {
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
                    entry.sourceData.pushAddressOffset = kHeapFrameAddrPushOffset + 0 * sizeof(uint64_t);
                    break;
                case 2: // lights
                case 3: // g_instances
                case 4: // g_joints
                case 5: // g_prevJoints
                case 6: // g_morphDeltas
                    entry.resourceMask                 = VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT;
                    entry.source                       = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
                    entry.sourceData.pushAddressOffset = kHeapFrameAddrPushOffset + (b.binding - 1) * sizeof(uint64_t);
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

std::expected<void, Error> RenderContext::Impl::BuildDecalPipeline() {
    using enum Resource::ShaderID;

    static constexpr std::array<VkFormat, 2> decalFormats = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM};

    auto decalShaders = Resource::GetShaderProgram(Decal);

    // Reflects decal.slang set 0 ({texDepth, pointSampler}) and set 1 (the scene
    // parameter block subset). VK_EXT_descriptor_heap: the reflection feeds the
    // mapping tables (decalHeapMappings + decalSceneHeapMappings) that remap
    // both sets onto the heaps at pipeline creation; no descriptor sets exist.
    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc(decalShaders.vertex), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc(decalShaders.fragment), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    if (!decalDescLayout.Build(ctx.Device(), std::span {reflectInputs})) {
        ZHLN::Log("[RenderInit] ERROR: Failed to reflect decal descriptor layout!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    BuildDecalHeapMappings();
    decalPipelineLayout = emptyPipelineLayout;

    // Merge decal set 0 + scene set 1 into one mapping chain per stage.
    std::vector<VkDescriptorSetAndBindingMappingEXT> mergedEntries;
    mergedEntries.insert(mergedEntries.end(), decalHeapMappings.entries.begin(), decalHeapMappings.entries.end());
    mergedEntries.insert(mergedEntries.end(), decalSceneHeapMappings.entries.begin(), decalSceneHeapMappings.entries.end());
    const VkShaderDescriptorSetAndBindingMappingInfoEXT mergedInfo = {
        .sType        = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
        .pNext        = nullptr,
        .mappingCount = static_cast<uint32_t>(mergedEntries.size()),
        .pMappings    = mergedEntries.empty() ? nullptr : mergedEntries.data(),
    };

    return LoadAndCreateShaders(
               {.path = Resource::Paths::DecalVS, .fallback = decalShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::DecalPS, .fallback = decalShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineBuilder<2, true> {} // Updated from 3 to 2 attachments
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&mergedInfo, &mergedInfo)
                .ColorFormats(decalFormats) // Explicit 2-format array
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .CullFront()
                .AlphaBlend()
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) { decalPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

std::expected<void, Error> RenderContext::Impl::BuildTAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, taaPass, "TAA", {.path = Resource::Paths::TaaVS, .fallback = Resource::GetShaderProgram(Taa).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::TaaPS, .fallback = Resource::GetShaderProgram(Taa).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildFXAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, fxaaPass, "FXAA", {.path = Resource::Paths::FxaaVS, .fallback = Resource::GetShaderProgram(Fxaa).vertex},
        {.path = Resource::Paths::FxaaPS, .fallback = Resource::GetShaderProgram(Fxaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildMLAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, mlaaPass, "MLAA", {.path = Resource::Paths::MlaaVS, .fallback = Resource::GetShaderProgram(Mlaa).vertex},
        {.path = Resource::Paths::MlaaPS, .fallback = Resource::GetShaderProgram(Mlaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildSMAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
               this, smaaEdgePass, "SMAA Edge Detection", {.path = Resource::Paths::SmaaEdgeVS, .fallback = Resource::GetShaderProgram(SmaaEdge).vertex},
               {.path = Resource::Paths::SmaaEdgePS, .fallback = Resource::GetShaderProgram(SmaaEdge).fragment}, {VK_FORMAT_R8G8_UNORM}
    )
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaWeightPass, "SMAA Blending Weight",
                {.path = Resource::Paths::SmaaWeightVS, .fallback = Resource::GetShaderProgram(SmaaWeight).vertex},
                {.path = Resource::Paths::SmaaWeightPS, .fallback = Resource::GetShaderProgram(SmaaWeight).fragment}, {VK_FORMAT_R8G8B8A8_UNORM}
            );
        })
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaBlendPass, "SMAA Neighborhood Blend",
                {.path = Resource::Paths::SmaaBlendVS, .fallback = Resource::GetShaderProgram(SmaaBlend).vertex},
                {.path = Resource::Paths::SmaaBlendPS, .fallback = Resource::GetShaderProgram(SmaaBlend).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
}

std::expected<void, Error> RenderContext::Impl::BuildAmbientPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, ambientPass, "Ambient", {.path = Resource::Paths::AmbientVS, .fallback = Resource::GetShaderProgram(Ambient).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::AmbientPS, .fallback = Resource::GetShaderProgram(Ambient).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildLightingPipeline() {
    using enum Resource::ShaderID;

    struct SpecData {
        int enableRTR;
    };
    std::array<VkSpecializationMapEntry, 1> specEntries = {{{.constantID = 0, .offset = offsetof(SpecData, enableRTR), .size = sizeof(int)}}};

    std::array<SpecData, 2>             variants = {{{.enableRTR = 0}, {.enableRTR = 1}}};
    std::array<VkSpecializationInfo, 2> specInfos {};
    for (int i = 0; i < 2; ++i) {
        specInfos[i] = {.mapEntryCount = 1, .pMapEntries = specEntries.data(), .dataSize = sizeof(SpecData), .pData = &variants[i]};
    }

    bool        hasRt  = rtCtx.Valid();
    const char* vsPath = hasRt ? Resource::Paths::LightingVS : Resource::Paths::LightingNortVS;
    const char* psPath = hasRt ? Resource::Paths::LightingPS : Resource::Paths::LightingNortPS;

    auto vsSpan = hasRt ? Resource::GetShaderProgram(Lighting).vertex : Resource::GetShaderProgram(LightingNort).vertex;
    auto psSpan = hasRt ? Resource::GetShaderProgram(Lighting).fragment : Resource::GetShaderProgram(LightingNort).fragment;

    return BuildPassVariants(
        this, lightingPass, "Lighting", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

std::expected<void, Error> RenderContext::Impl::BuildReflectionPipelines() {
    using enum Resource::ShaderID;

    struct SpecData {
        int enableSSR;
        int enableRTR;
    };
    std::array<VkSpecializationMapEntry, 2> specEntries = {
        {{.constantID = 0, .offset = offsetof(SpecData, enableSSR), .size = sizeof(int)},
         {.constantID = 1, .offset = offsetof(SpecData, enableRTR), .size = sizeof(int)}}
    };

    std::array<SpecData, 4> variants = {
        {{.enableSSR = 0, .enableRTR = 0}, {.enableSSR = 1, .enableRTR = 0}, {.enableSSR = 0, .enableRTR = 1}, {.enableSSR = 1, .enableRTR = 1}}
    };
    std::array<VkSpecializationInfo, 4> specInfos {};
    for (int i = 0; i < 4; ++i) {
        specInfos[i] = {.mapEntryCount = 2, .pMapEntries = specEntries.data(), .dataSize = sizeof(SpecData), .pData = &variants[i]};
    }

    bool        hasRt  = rtCtx.Valid();
    const char* vsPath = hasRt ? Resource::Paths::ReflectionVS : Resource::Paths::ReflectionNortVS;
    const char* psPath = hasRt ? Resource::Paths::ReflectionPS : Resource::Paths::ReflectionNortPS;

    auto vsSpan = hasRt ? Resource::GetShaderProgram(Reflection).vertex : Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).vertex;
    auto psSpan = hasRt ? Resource::GetShaderProgram(Reflection).fragment : Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).fragment;

    auto res = BuildPassVariants(
        this, reflectionPass, "Reflection", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
    if (!res) {
        return res;
    }

    return BuildPassVariants(
        this, translucentReflectionPass, "Translucent Reflection", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

std::expected<void, Error> RenderContext::Impl::BuildBloomPipelines() {
    using enum Resource::ShaderID;

    auto res = BuildPassHelper(
        this, bloomThresholdPass, "Bloom Threshold", {.path = Resource::Paths::BloomThresholdVS, .fallback = Resource::GetShaderProgram(BloomThreshold).vertex},
        {.path = Resource::Paths::BloomThresholdPS, .fallback = Resource::GetShaderProgram(BloomThreshold).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );

    for (int i = 0; i < 3; ++i) {
        res = res.and_then(
                     [&, i]() {
                         std::string downName = std::format("Bloom Downsample {}", i);
                         return BuildPassHelper(
                             this, bloomDownPass[i], downName.c_str(),
                             {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                             {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
                         );
                     }
        ).and_then([&, i]() {
            std::string upName = std::format("Bloom Upsample {}", i);
            return BuildPassHelper(
                this, bloomUpPass[i], upName.c_str(), {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
    }

    return res;
}

std::expected<void, Error> RenderContext::Impl::BuildBlitPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, blitPass, "Blit", {.path = Resource::Paths::BlitVS, .fallback = Resource::GetShaderProgram(Blit).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::BlitPS, .fallback = Resource::GetShaderProgram(Blit).fragment, .entryPoint = "PSMain"}, {presentation.GetPresentFormat()}
    );
}

void RenderContext::Impl::InitPassSamplerDescriptors() noexcept {
    // Write the static sampler descriptors of every descriptor-heap pass into
    // their allocated sampler-heap slots (each pass baked its own slot at
    // pipeline-build time). Sampler ORDER per pass mirrors each pass's set-0
    // declaration order (sampler positions only).
    const VkSamplerCreateInfo defaultInfo = defaultSamplerInfo;
    const VkSamplerCreateInfo pointInfo   = pointSamplerInfo;
    const VkSamplerCreateInfo shadowInfo  = shadowSamplerInfo;
    const VkSamplerCreateInfo clampInfo   = [&]() {
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
        std::array<VkSamplerCreateInfo, 2> infos = {defaultInfo, pointInfo};
        Vk::InitHeapPassSamplers(heapManager, ambientPass.heapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 4> infos = {defaultInfo, shadowInfo, clampInfo, pointInfo};
        Vk::InitHeapPassSamplers(heapManager, lightingPass.heapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 3> infos = {defaultInfo, pointInfo, clampInfo};
        Vk::InitHeapPassSamplers(heapManager, reflectionPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, translucentReflectionPass.heapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 1> infos = {defaultInfo};
        Vk::InitHeapPassSamplers(heapManager, taaPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, fxaaPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, mlaaPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, smaaEdgePass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, smaaWeightPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, smaaBlendPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, bloomThresholdPass.heapBindings, infos);
        for (auto& kawase: bloomDownPass) {
            Vk::InitHeapPassSamplers(heapManager, kawase.heapBindings, infos);
        }
        for (auto& kawase: bloomUpPass) {
            Vk::InitHeapPassSamplers(heapManager, kawase.heapBindings, infos);
        }
        Vk::InitHeapPassSamplers(heapManager, blitPass.heapBindings, infos);
        Vk::InitHeapPassSamplers(heapManager, volumetricTemporalPass.heapBindings, infos);
    }
    {
        std::array<VkSamplerCreateInfo, 1> infos = {shadowInfo};
        Vk::InitHeapPassSamplers(heapManager, volumetricLightInjectPass.heapBindings, infos);
    }
}

std::expected<void, Error> RenderContext::Impl::InitPostProcessing() {
    auto register_and_check = [&](const char* name, auto&& build_fn, std::initializer_list<const char*> watchPaths) -> std::expected<void, Error> {
        auto res = build_fn();
        if (!res) {
            ZHLN::Log("Pipeline '{}' failed to compile: {}", name, res.error().Message());
            return std::unexpected(res.error());
        }
        if constexpr (isDev) {
            for (const auto* path: watchPaths) {
                RegisterShaderWatcher(path, [=, build_fn = std::forward<decltype(build_fn)>(build_fn)]() {
                    auto reload_res = build_fn();
                    if (!reload_res) {
                        ZHLN::Log("ERROR: Failed to hot-reload pipeline '{}': {}", name, reload_res.error().Message());
                    } else {
                        ZHLN::Log("[Shader Reload] Pipeline '{}' hot-reloaded successfully.", name);
                    }
                });
            }
        }
        return {};
    };

    auto buildVolumetrics = [&]() -> std::expected<void, Error> {
        auto csClear = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricClear).vertex);
        if (!volumetricClearPass.BuildHeap(ctx.Device(), heapManager, csClear)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        auto csFogInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricFogInject).vertex);
        if (!volumetricFogInjectPass.BuildHeap(ctx.Device(), heapManager, csFogInject)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        auto csLightInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricLightInject).vertex);
        if (!volumetricLightInjectPass.BuildHeap(ctx.Device(), heapManager, csLightInject)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        auto csIntegrate = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricIntegration).vertex);
        if (!volumetricIntegrationPass.BuildHeap(ctx.Device(), heapManager, csIntegrate)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        auto csTemporal = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricTemporal).vertex);
        if (!volumetricTemporalPass.BuildHeap(ctx.Device(), heapManager, csTemporal)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        return {};
    };

    auto defaultSamplerBuilder = Vk::SamplerBuilder {}.Linear().ClampToEdge();
    return defaultSamplerBuilder.Build(ctx.Device())
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&](auto defaultResult) -> std::expected<void, Error> {
            defaultSampler     = std::move(defaultResult);
            defaultSamplerInfo = defaultSamplerBuilder.Info();
            auto pointBuilder  = Vk::SamplerBuilder {}.Nearest().ClampToEdge();
            return pointBuilder.Build(ctx.Device()).transform_error([](auto err) -> Error { return err; }).transform([&](auto pointResult) {
                pointSampler     = std::move(pointResult);
                pointSamplerInfo = pointBuilder.Info();
                // The decal pass samples through the sampler heap: write the
                // point-sampler descriptor into its static heap slot.
                WritePointSamplerToHeap(pointBuilder.Info());
            });
        })
        .and_then([&]() { return register_and_check("TAA", [this]() { return BuildTAAPipeline(); }, {Resource::Paths::TaaVS, Resource::Paths::TaaPS}); })
        .and_then([&]() { return register_and_check("FXAA", [this]() { return BuildFXAAPipeline(); }, {Resource::Paths::FxaaVS, Resource::Paths::FxaaPS}); })
        .and_then([&]() { return register_and_check("MLAA", [this]() { return BuildMLAAPipeline(); }, {Resource::Paths::MlaaVS, Resource::Paths::MlaaPS}); })
        .and_then([&]() {
            return register_and_check(
                "SMAA", [this]() { return BuildSMAAPipeline(); },
                {Resource::Paths::SmaaEdgeVS, Resource::Paths::SmaaEdgePS, Resource::Paths::SmaaWeightVS, Resource::Paths::SmaaWeightPS,
                 Resource::Paths::SmaaBlendVS, Resource::Paths::SmaaBlendPS}
            );
        })
        .and_then([&]() {
            return register_and_check("Ambient", [this]() { return BuildAmbientPipeline(); }, {Resource::Paths::AmbientVS, Resource::Paths::AmbientPS});
        })
        .and_then([&]() {
            return register_and_check(
                "Lighting", [this]() { return BuildLightingPipeline(); },
                {Resource::Paths::LightingVS, Resource::Paths::LightingPS, Resource::Paths::LightingNortVS, Resource::Paths::LightingNortPS}
            );
        })
        .and_then([&]() {
            return register_and_check(
                "Reflection", [this]() { return BuildReflectionPipelines(); },
                {Resource::Paths::ReflectionVS, Resource::Paths::ReflectionPS, Resource::Paths::ReflectionNortVS, Resource::Paths::ReflectionNortPS}
            );
        })
        .and_then([&]() {
            return register_and_check(
                "Bloom", [this]() { return BuildBloomPipelines(); },
                {Resource::Paths::BloomThresholdVS, Resource::Paths::BloomThresholdPS, Resource::Paths::BloomBlurVS, Resource::Paths::BloomBlurPS}
            );
        })
        .and_then([&]() {
            return register_and_check(
                "Volumetrics", buildVolumetrics,
                {Resource::Paths::VolumetricClearCS, Resource::Paths::VolumetricFogInjectCS, Resource::Paths::VolumetricLightInjectCS,
                 Resource::Paths::VolumetricIntegrationCS, Resource::Paths::VolumetricTemporalCS}
            );
        })
        .and_then([&]() {
            // Build Particle Compute + Render Pipelines
            return register_and_check(
                "Particles", [this]() { return BuildParticlePipelines(); },
                {Resource::Paths::ParticleUpdateCS, Resource::Paths::ParticleRenderVS, Resource::Paths::ParticleRenderPS}
            );
        })
        .and_then([&]() {
            return register_and_check(
                "3D Mesh Particles", [this]() { return BuildMeshParticlePipelines(); },
                {Resource::Paths::MeshParticleUpdateCS, Resource::Paths::MeshParticleRenderVS, Resource::Paths::MeshParticleRenderPS,
                 Resource::Paths::MeshParticleShadowVS}
            );
        })
        .and_then([&]() {
            return register_and_check("Decals", [this]() { return BuildDecalPipeline(); }, {Resource::Paths::DecalVS, Resource::Paths::DecalPS});
        })
        .and_then([&]() { return register_and_check("Blit", [this]() { return BuildBlitPipeline(); }, {Resource::Paths::BlitVS, Resource::Paths::BlitPS}); })
        // CHANGED: Converted to .and_then to handle expected texture allocations monadically
        .and_then([&]() -> std::expected<void, Error> {
            ZHLN::Array<uint32_t> smaaAreaPixels(static_cast<size_t>(160 * 560));
            ZHLN::PBR::FillSmaaAreaTex(smaaAreaPixels);
            ZHLN::Array<uint32_t> smaaSearchPixels(static_cast<size_t>(64 * 16));
            ZHLN::PBR::FillSmaaSearchTex(smaaSearchPixels);

            return CreateTextureInternal(smaaAreaPixels.data(), 160, 560, false)
                .and_then([&, smaaSearchPixels](uint32_t areaIdx) -> std::expected<void, Error> {
                    smaaAreaTexIdx = areaIdx;
                    return CreateTextureInternal(smaaSearchPixels.data(), 64, 16, false).transform([&](uint32_t searchIdx) -> void {
                        smaaSearchTexIdx = searchIdx;
                    });
                });
        })
        .and_then([&]() -> std::expected<void, Error> {
            // Every pass pipeline + binding table now exists: write the static
            // sampler descriptors into their heap slots.
            InitPassSamplerDescriptors();
            return {};
        });
}

std::expected<void, Error> RenderContext::Impl::InitCSGPipelines() {
    using enum Resource::ShaderID;

    // We declare the shared shaders in a stack variable so all lambdas can reference it.
    Vk::ShaderStages shaders;

    auto basicShaders = Resource::GetShaderProgram(Basic);

    return LoadAndCreateShaders(
               {.path = Resource::Paths::BasicVS, .fallback = basicShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::BasicPS, .fallback = basicShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& compiledShaders) {
            shaders = std::forward<decltype(compiledShaders)>(compiledShaders);

            csgPipelineLayout             = emptyPipelineLayout;
            VkStencilOpState writeStencil = {
                .failOp      = VK_STENCIL_OP_KEEP,
                .passOp      = VK_STENCIL_OP_REPLACE,
                .depthFailOp = VK_STENCIL_OP_KEEP,
                .compareOp   = VK_COMPARE_OP_ALWAYS,
                .compareMask = 0xFF,
                .writeMask   = 0xFF,
                .reference   = 1
            };

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .CullNone()
                .ColorWriteEnable(false)
                .StencilTest(true)
                .StencilOp(writeStencil, writeStencil)
                .Build(ctx.Device())
                .transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&](auto&& writePipeline) {
            csgWritePipeline = std::forward<decltype(writePipeline)>(writePipeline);

            VkStencilOpState diffStencil = {
                .failOp      = VK_STENCIL_OP_KEEP,
                .passOp      = VK_STENCIL_OP_KEEP,
                .depthFailOp = VK_STENCIL_OP_KEEP,
                .compareOp   = VK_COMPARE_OP_NOT_EQUAL,
                .compareMask = 0xFF,
                .writeMask   = 0x00,
                .reference   = 1
            };

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .ColorWriteEnable(true)
                .StencilTest(true)
                .StencilOp(diffStencil, diffStencil)
                .Build(ctx.Device())
                .transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&](auto&& diffPipeline) {
            csgDifferencePipeline = std::forward<decltype(diffPipeline)>(diffPipeline);

            VkStencilOpState intersectStencil = {
                .failOp      = VK_STENCIL_OP_KEEP,
                .passOp      = VK_STENCIL_OP_KEEP,
                .depthFailOp = VK_STENCIL_OP_KEEP,
                .compareOp   = VK_COMPARE_OP_EQUAL,
                .compareMask = 0xFF,
                .writeMask   = 0x00,
                .reference   = 1
            };

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .ColorWriteEnable(true)
                .StencilTest(true)
                .StencilOp(intersectStencil, intersectStencil)
                .Build(ctx.Device())
                .transform_error([](auto e) -> Error { return e; });
        })
        .transform([&](auto&& intersectPipeline) {
            csgIntersectionPipeline = std::forward<decltype(intersectPipeline)>(intersectPipeline);

            WatchPipeline(Resource::Paths::BasicVS, Resource::Paths::BasicPS, [this]() {
                auto res = InitCSGPipelines();
                if (!res) {
                    ZHLN::Log("ERROR: Failed to hot-reload CSG stencil pipelines: {}", res.error().Message());
                } else {
                    ZHLN::Log("[Shader Reload] CSG Stencil pipelines hot-reloaded successfully.");
                }
            });
        });
}

std::expected<void, Error> RenderContext::Impl::SetupUI(GLFWwindow* window) {
    using enum Resource::ShaderID;
    auto make_expected = [](bool success, Error err) -> std::expected<void, Error> {
        if (success) {
            return {};
        }
        return std::unexpected(err);
    };

    Vk::ShaderStages uiShaders;

    // VK_EXT_descriptor_heap: ImGui renders through the heaps via the
    // imgui_impl_vulkan_heap fork; no descriptor pool exists anymore. Reserve
    // two static sampler slots for the backend's linear/nearest samplers.
    auto imguiSamplerLinear  = heapManager.AllocateStaticSampler();
    auto imguiSamplerNearest = heapManager.AllocateStaticSampler();
    if (!imguiSamplerLinear || !imguiSamplerNearest) {
        return std::unexpected(RenderInitError::UISetupFailed);
    }

    return Vk::ShaderStages::Create(ctx.Device(), Resource::GetShaderProgram(Ui))
        .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            uiShaders = std::forward<decltype(shaders)>(shaders);
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            // The UI batch pipeline is a descriptor-heap pipeline (scene
            // registry + push data). ImGui renders through the heaps too
            // (rendered last, after all other passes).
            uiPipelineLayout = emptyPipelineLayout;

            VkFormat swapchainFormat = presentation.GetPresentFormat();

            return Vk::PipelineBuilder {}
                .Shaders(uiShaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(std::array {swapchainFormat})
                .NoDepth()
                .AlphaBlend()
                .CullNone()
                .Build(ctx.Device())
                .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
                .transform([&](auto&& pipeline) { uiPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (window != nullptr) {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGui_ImplGlfw_InitForVulkan(window, true);

                VkFormat swapchainFormat = presentation.GetPresentFormat();

                ImGui_ImplVulkanHeap_InitInfo init_info = {
                    .ApiVersion         = VK_API_VERSION_1_3,
                    .Instance           = ctx.Instance(),
                    .PhysicalDevice     = ctx.Physical(),
                    .Device             = ctx.Device(),
                    .QueueFamily        = ctx.PhysicalInfo().graphics_family,
                    .Queue              = ctx.GraphicsQueue(),
                    .DescriptorPool     = VK_NULL_HANDLE,
                    .DescriptorPoolSize = 0,
                    .MinImageCount      = 2,
                    .ImageCount         = 2,
                    .PipelineCache      = VK_NULL_HANDLE,
                    .PipelineInfoMain =
                        {
                            .RenderPass  = VK_NULL_HANDLE,
                            .Subpass     = 0,
                            .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
                            .ExtraDynamicStates {},
                            .PipelineRenderingCreateInfo =
                                {.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                 .pNext                   = nullptr,
                                 .viewMask                = 0,
                                 .colorAttachmentCount    = 1,
                                 .pColorAttachmentFormats = &swapchainFormat,
                                 .depthAttachmentFormat   = VK_FORMAT_D32_SFLOAT_S8_UINT,
                                 .stencilAttachmentFormat = VK_FORMAT_UNDEFINED},
                        },
                    .UseDynamicRendering        = true,
                    .Allocator                  = nullptr,
                    .CheckVkResultFn            = nullptr,
                    .MinAllocationSize          = 0,
                    .CustomShaderVertCreateInfo = {},
                    .CustomShaderFragCreateInfo = {},
                    .HeapInfo                   = {
                        .HeapContext        = &ctx,
                        .HeapManager        = &heapManager,
                        .ResourceSlotBase   = imguiTextureHeapBase,
                        .ResourceSlotCount  = kImGuiTextureSlots,
                        .ResourceStride     = static_cast<uint32_t>(heapManager.ResourceStride()),
                        .SamplerSlotLinear  = imguiSamplerLinear->index,
                        .SamplerSlotNearest = imguiSamplerNearest->index,
                        .SamplerStride      = static_cast<uint32_t>(heapManager.SamplerStride()),
                    },
                };

                return make_expected(ImGui_ImplVulkanHeap_Init(&init_info), RenderInitError::UISetupFailed);
            }
            return {};
        });
}

void RenderContext::Impl::RecreatePunctualShadowViews() noexcept {
    punctualShadowViews.clear();
    punctualShadowViews.resize(MAX_PUNCTUAL_LIGHTS);
    for (uint32_t i = 0; i < MAX_PUNCTUAL_LIGHTS; ++i) {
        auto view_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(
            ctx.Device(), graphResources.shadowAtlas.image.Handle(),
            i * 6,                    // baseLayer
            6,                        // layerCount
            VK_IMAGE_ASPECT_DEPTH_BIT // aspect
        );
        if (view_res.has_value()) {
            punctualShadowViews[i] = std::move(*view_res);
        }
    }
}

std::expected<void, Error> RenderContext::Impl::InitSkeletalAnimationResources() {
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

namespace {

// Tags every long-lived image with its semantic name via VK_EXT_debug_utils so
// validation messages and captures identify resources instead of raw handles.
// No-op unless validation is enabled (the extension is only injected then).
void ApplyImageDebugNames(RenderContext::Impl& impl) noexcept {
    const auto& ctx = impl.ctx;

    Reflect::ForEachReflectedField<typename RenderContext::Impl::GraphResources::ReflectMetadata>(impl.graphResources, [&]<typename Tag>(auto& rt) {
        if constexpr (requires { rt.image.Handle(); }) {
            Vk::Debug::SetImageName(ctx, rt.image.Handle(), Tag::name.string_view());
        }
    });

    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[0].image.Handle(), "AccumHistory0");
    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[1].image.Handle(), "AccumHistory1");
    Vk::Debug::SetImageName(ctx, impl.presentation.depthTarget.image.Handle(), "DepthTarget");
    Vk::Debug::SetImageName(ctx, impl.shadowMapPrev.image.Handle(), "ShadowMapPrev");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.brdfLutImage.Handle(), "IBL.BrdfLut");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.prefilteredImage.Handle(), "IBL.PrefilteredCube");
    Vk::Debug::SetImageName(ctx, impl.ltcMatImage.Handle(), "LTC.Mat");
    Vk::Debug::SetImageName(ctx, impl.ltcAmpImage.Handle(), "LTC.Amp");

    for (size_t i = 0; i < impl.textureImages.size(); ++i) {
        Vk::Debug::SetImageName(ctx, impl.textureImages[i].Handle(), std::format("BindlessTexture{:03}", i));
    }

    const auto& swapchain = impl.presentation.swapchain.Get();
    for (uint32_t i = 0; i < swapchain.image_count; ++i) {
        Vk::Debug::SetImageName(ctx, swapchain.images[i], std::format("Swapchain{}", i));
    }
}

} // namespace

std::expected<void, Error> RenderContext::Impl::InitLightingLUTs() {
    stagingContext = std::make_unique<Vk::StagingContext>(allocator, ctx);

    using namespace Resource;
    const size_t matRawSize = ltc_mat.size() - 128;
    const size_t ampRawSize = ltc_amp.size() - 128;

    return stagingContext->Begin()
        .and_then([&]() { return Vk::IBLProcessor::Bake(*this, *stagingContext).transform_error([](auto res) -> Error { return res; }); })
        .and_then([&, matRawSize, ampRawSize](auto&& ibl) {
            iblPayload = std::forward<decltype(ibl)>(ibl);
            ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

            return Vk::Buffer::Create(allocator.Get(), matRawSize + ampRawSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
                .transform_error([](auto res) -> Error { return res; });
        })
        .and_then([&, matRawSize](auto&& ltcStaging) {
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
                .and_then([&, ltcInfo, ltcStaging = std::forward<decltype(ltcStaging)>(ltcStaging), matRawSize](auto&& matImg) mutable {
                    return Vk::Image::Create(allocator.Get(), ltcInfo, VMA_MEMORY_USAGE_GPU_ONLY)
                        .transform_error([](auto res) -> Error { return res; })
                        .transform([&, matImg = std::forward<decltype(matImg)>(matImg), ltcStaging = std::move(ltcStaging), matRawSize](auto&& ampImg) mutable {
                            stagingContext->UploadImage2DBuffer(matImg.Handle(), 64, 64, 1, ltcStaging.Handle(), 0);
                            stagingContext->UploadImage2DBuffer(ampImg.Handle(), 64, 64, 1, ltcStaging.Handle(), matRawSize);

                            stagingContext->AddBuffer(std::move(ltcStaging));
                            return std::make_pair(std::move(matImg), std::forward<decltype(ampImg)>(ampImg));
                        });
                });
        })
        .transform([&](auto&& images) -> void {
            ltcMatImage = std::move(images.first);
            ltcAmpImage = std::move(images.second);

            stagingContext->ExecuteAsync();

            if (auto mat_view = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcMatImage.Handle())) {
                ltcMatView = std::move(*mat_view);
            }
            if (auto amp_view = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcAmpImage.Handle())) {
                ltcAmpView = std::move(*amp_view);
            }
            ltcMatViewInfo = Vk::MakeViewCreateInfo2D(ltcMatImage.Handle(), VK_FORMAT_R16G16B16A16_SFLOAT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
            ltcAmpViewInfo = Vk::MakeViewCreateInfo2D(ltcAmpImage.Handle(), VK_FORMAT_R16G16B16A16_SFLOAT, 1, VK_IMAGE_ASPECT_COLOR_BIT);

            ApplyImageDebugNames(*this);
        });
}

std::expected<void, Error> RenderContext::Impl::RecreateTargets(VkExtent2D ext) {
    if (!presentation.Rebuild(ext.width, ext.height)) {
        return std::unexpected(RenderInitError::PresentationFailed);
    }

    auto assign_target = [this](auto& member, auto expected) -> std::expected<void, Error> {
        if (!expected) {
            return std::unexpected(RenderInitError::SubsystemAllocationFailed);
        }
        member = std::move(*expected);
        return {};
    };

    auto assign_target3d = [this](auto& member, auto expected) -> std::expected<void, Error> {
        if (!expected) {
            return std::unexpected(RenderInitError::SubsystemAllocationFailed);
        }
        member = std::move(*expected);
        return {};
    };

    return assign_target(graphResources.sceneColor, CreateDefaultTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>(ext))
        .and_then([&]() { return assign_target(graphResources.velocityBuffer, CreateDefaultTarget<VK_FORMAT_R16G16_SFLOAT>(ext)); })
        .and_then([&]() { return assign_target(frames.accumBuffers[0], CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_DST_BIT)); })
        .and_then([&]() { return assign_target(frames.accumBuffers[1], CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_DST_BIT)); })
        .and_then([&]() { return assign_target(graphResources.normalRoughnessBuffer, CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext)); })
        .and_then([&]() { return assign_target(graphResources.hdrSceneColor, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_SRC_BIT)); })
        .and_then([&]() { return assign_target(graphResources.ambientTarget, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext)); })
        .and_then([&]() { return assign_target(graphResources.lightingTarget, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext)); })
        .and_then([&]() { return assign_target(graphResources.smaaEdgeTarget, CreateDefaultTarget<VK_FORMAT_R8G8_UNORM>(ext)); })
        .and_then([&]() { return assign_target(graphResources.smaaWeightTarget, CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext)); })

    .and_then([&]() {
            VkExtent2D ext2  = {.width = std::max(1u, ext.width / 2), .height = std::max(1u, ext.height / 2)};
            VkExtent2D ext4  = {.width = std::max(1u, ext.width / 4), .height = std::max(1u, ext.height / 4)};
            VkExtent2D ext8  = {.width = std::max(1u, ext.width / 8), .height = std::max(1u, ext.height / 8)};
            VkExtent2D ext16 = {.width = std::max(1u, ext.width / 16), .height = std::max(1u, ext.height / 16)};
            return assign_target(graphResources.bloomThresholdTarget, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2));
        })
        .and_then([&]() { return assign_target(graphResources.bloomDown1, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4)); })
        .and_then([&]() { return assign_target(graphResources.bloomDown2, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8)); })
        .and_then([&]() { return assign_target(graphResources.bloomDown3, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext16)); })
        .and_then([&]() { return assign_target(graphResources.bloomUp2, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8)); })
        .and_then([&]() { return assign_target(graphResources.bloomUp1, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4)); })
        .and_then([&]() { return assign_target(graphResources.bloomFinalTarget, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2)); })
        .and_then([&]() { return assign_target(graphResources.transNormalBuffer, CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext)); })
        .and_then([&]() { return assign_target(graphResources.transLightingTarget, CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext)); })
        .and_then([&]() {
            return assign_target(graphResources.transDepthBuffer,
                Vk::RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
                    allocator, ctx, ext, {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
                ));
        })
        .and_then([&]() {
            return assign_target(graphResources.hizMap,
                Vk::MipmappedRenderTarget<VK_FORMAT_R32_SFLOAT>::Create(
                    allocator, ctx, ext,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT
                ));
        })
        .transform([&]() { RecreatePunctualShadowViews(); })

    .transform([&]() {
            RecreatePunctualShadowViews();

            // Transition all newly allocated render targets to their correct default layouts
            Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
                // History-bearing targets are READ before their first full-coverage
                // write: TAA samples AccumCurr on frame 0, and the volumetric
                // temporal filter samples VoxelHist before it ever wrote it (and the
                // graphics queue reads VoxelResolved one compute-submission early).
                // Leaving the content as VRAM garbage made the very first frames
                // differ between runs — worse, NaN bit patterns survive the
                // neighborhood clamps and poison temporal accumulation indefinitely.
                // Clear every target whose first definition is a read.
                const VkClearColorValue       clearBlack = {.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};
                const VkImageSubresourceRange clearRange = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = 0,
                    .levelCount     = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount     = VK_REMAINING_ARRAY_LAYERS
                };
                const std::array accumImages = {frames.accumBuffers[0].image.Handle(), frames.accumBuffers[1].image.Handle()};
                for (const auto img: accumImages) {
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
                    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearBlack, 1, &clearRange);
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
                }
                const std::array targets3D = {
                    graphResources.voxelMedia.image.Handle(), graphResources.voxelLight.image.Handle(), graphResources.voxelIntegrated.image.Handle(),
                    graphResources.voxelHistory.image.Handle(), graphResources.voxelResolved.image.Handle()
                };
                for (auto* const img: targets3D) {
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
                    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearBlack, 1, &clearRange);
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
                }

                std::array colorTargets = {graphResources.sceneColor.image.Handle(),
                                           graphResources.velocityBuffer.image.Handle(),
                                           graphResources.normalRoughnessBuffer.image.Handle(),
                                           graphResources.hdrSceneColor.image.Handle(),
                                           graphResources.ambientTarget.image.Handle(),
                                           graphResources.lightingTarget.image.Handle(),
                                           graphResources.smaaEdgeTarget.image.Handle(),
                                           graphResources.smaaWeightTarget.image.Handle(),
                                           graphResources.bloomThresholdTarget.image.Handle(),
                                           graphResources.bloomDown1.image.Handle(),
                                           graphResources.bloomDown2.image.Handle(),
                                           graphResources.bloomDown3.image.Handle(),
                                           graphResources.bloomUp2.image.Handle(),
                                           graphResources.bloomUp1.image.Handle(),
                                           graphResources.bloomFinalTarget.image.Handle(),
                                           graphResources.transNormalBuffer.image.Handle(),
                                           graphResources.transLightingTarget.image.Handle()};

                for (auto* const img: colorTargets) {
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
                }

                // VK_EXT_descriptor_heap: the decal pass samples the depth target
                // through the heap, so rewrite its descriptor whenever the target is
                // recreated (the old view was destroyed). Depth/stencil sampled-image
                // descriptors must select exactly one aspect (VUID-VkImageDescriptorInfoEXT-pView-11430);
                // decal.slang only reads the depth value.
                if (decalDepthSlot.Valid()) {
                    const auto info = Vk::MakeViewCreateInfo2D(presentation.depthTarget.image.Handle(), VK_FORMAT_D32_SFLOAT_S8_UINT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
                    heapManager.WriteImage(decalDepthSlot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
                    cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
                    cmd, graphResources.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                );

                const VkClearColorValue clearFarDepth = {.float32 = {1.0F, 1.0F, 1.0F, 1.0F}};
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
                    cmd, graphResources.hizMap.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT
                );
                vkCmdClearColorImage(cmd, graphResources.hizMap.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearFarDepth, 1, &clearRange);
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.hizMap.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT
                );
            });

            // Translucency input is a plain Texture2D at registry slot 10 (sampler split).
            // VK_EXT_descriptor_heap: rewrite its static heap descriptor for the new target.
            WriteTransLightingToHeap();

            // VK_EXT_descriptor_heap: rewrite the Hi-Z descriptor slots.
            const uint32_t mips = std::min<uint32_t>(graphResources.hizMap.mipLevels, 16);
            for (uint32_t m = 0; m < mips; ++m) {
                const Vk::TypedImage<VK_IMAGE_LAYOUT_GENERAL> outMip {
                    .handle   = graphResources.hizMap.image.Handle(),
                    .view     = graphResources.hizMap.mipViews[m].Get(),
                    .extent   = {.width = graphResources.hizMap.extent.width, .height = graphResources.hizMap.extent.height, .depth = 1},
                    .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                    .format   = VK_FORMAT_R32_SFLOAT,
                    .viewInfo = &graphResources.hizMap.mipViewInfos[m]
                };
                if (m == 0) {
                    Vk::WriteHeapBindings(
                        heapManager, ctx, hizHeapBindings, m, Vk::Assume<Vk::ComputeRead<Res_Depth>>(presentation.depthTarget), outMip, Vk::SkipWrite {}
                    );
                } else {
                    const Vk::TypedImage<VK_IMAGE_LAYOUT_GENERAL> inMip {
                        .handle   = graphResources.hizMap.image.Handle(),
                        .view     = graphResources.hizMap.mipViews[m - 1].Get(),
                        .extent   = {.width = graphResources.hizMap.extent.width, .height = graphResources.hizMap.extent.height, .depth = 1},
                        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                        .format   = VK_FORMAT_R32_SFLOAT,
                        .viewInfo = &graphResources.hizMap.mipViewInfos[m - 1]
                    };
                    Vk::WriteHeapBindings(heapManager, ctx, hizHeapBindings, m, inMip, outMip, Vk::SkipWrite {});
                }
            }

            // Culling: one slot span per {pass 0/1} x {frame parity}.
            for (uint32_t idx = 0; idx < 4; ++idx) {
                const uint32_t pass     = idx >> 1;
                const uint32_t parity   = idx & 1;
                const auto&    indirect = (pass == 0) ? frames.indirectCommandsBuffers[parity] : frames.indirectCommandsBuffersPass2[parity];
                Vk::WriteHeapBindings(
                    heapManager, ctx, cullingHeapBindings, idx, frames.instanceDataBuffers[parity], indirect,
                    Vk::Assume<Vk::ComputeRead<Res_HiZ>>(graphResources.hizMap), Vk::SkipWrite {}, frames.secondPassCandidatesBuffers[parity],
                    frames.secondPassCountBuffers[parity]
                );
            }

            ApplyImageDebugNames(*this);
        });
}

std::expected<void, Error> RenderContext::Impl::BuildHangGpuPipeline() {
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            hangGpuPass.pipelineLayout = std::forward<decltype(layout)>(layout);
            return LoadAndCreateComputeShader(
                       ComputeStageSource {.path = Resource::Paths::HangGpuCS, .fallback = Resource::hang_gpu_comp}, hangGpuPass.pipelineLayout.Get()
            )
                .transform([&](auto&& pipeline) { hangGpuPass.pipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

std::expected<void, Error> RenderContext::Impl::BuildHiZPipeline() {
    auto shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::HizGenerateComp).vertex);
    if (!hizDescLayout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        ZHLN::Log("[RenderInit] ERROR: Failed to reflect Hi-Z layout from hiz_generate SPV!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // VK_EXT_descriptor_heap: one slot span per mip (the pushed index is the
    // mip level). The span is fixed at 16: the HiZ map does not exist yet at
    // pipeline-build time (it is created on the first RecreateTargets).
    constexpr uint32_t kMaxHiZMips = 16;
    Vk::BuildHeapPassBindings(heapManager, hizDescLayout.reflectedSets[0], 0, Vk::kHeapIndexPushOffset, kMaxHiZMips, hizHeapBindings);

    return hizGeneratePass.BuildHeap(ctx.Device(), shader, hizHeapBindings.GetInfo());
}

std::expected<void, Error> RenderContext::Impl::InitUIDynamicBuffers() noexcept {
    return AllocateDynamicVertexBuffers(kMaxUiVertices, frames.uiVbos, frames.uiVboAddresses, 0, "UI");
}

} // namespace ZHLN
