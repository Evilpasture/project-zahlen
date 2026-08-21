// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderInit.cpp
#include "IBLProcessor.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "SMAALUTGenerator.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "../TTYBackend.hpp"
#include "imgui.h"
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

HardwareCaps ProbeHardware(VkPhysicalDevice physicalDevice, uint32_t apiVersion) noexcept {
    HardwareCaps caps {};
    HardwareCapsProber(physicalDevice, apiVersion).ProbeInt64(caps.supportsInt64).ProbeDrawIndirectCount(caps.supportsDrawIndirectCount);
    return caps;
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
    auto csShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ParticleUpdate).vertex);

    VkPushConstantRange updatePush = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(ComputePushConstants) // 176 Bytes
    };

    if (!particleUpdatePass.Build(ctx.Device(), bindlessLayout.GetSetLayout(), csShader, &updatePush, 1)) {
        ZHLN::Log("ERROR: Failed to build particle update compute pipeline!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // 3. Build Billboard Graphics Pipeline (particle_render.hlsl)
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .AddDescriptorSetLayout(bindlessLayout.GetSetLayout())
        .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ParticleRenderPushConstants))
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            particleRenderLayout = std::forward<decltype(layout)>(layout);

            auto renderShaders = Resource::GetShaderProgram(ParticleRender);
            return LoadAndCreateShaders(
                       {.path = Resource::Paths::ParticleRenderVS, .fallback = renderShaders.vertex, .entryPoint = "VSMain"},
                       {.path = Resource::Paths::ParticleRenderPS, .fallback = renderShaders.fragment, .entryPoint = "PSMain"}
            )
                .and_then([&](auto&& shaders) -> std::expected<void, Error> {
                    return Vk::PipelineBuilder {}
                        .Shaders(shaders)
                        .Layout(particleRenderLayout.Get())
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
        });
}

std::expected<void, Error> RenderContext::Impl::BuildMeshParticlePipelines() {
    using enum Resource::ShaderID;

    // 1. Compute Simulation Pipeline (mesh_particle_update.hlsl)
    auto                csMeshShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(MeshParticleUpdate).vertex);
    VkPushConstantRange mpUpdatePush = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(RenderContext::Impl::MeshParticleComputePush) // 176 bytes
    };

    if (!meshParticleUpdatePass.Build(ctx.Device(), bindlessLayout.GetSetLayout(), csMeshShader, &mpUpdatePush, 1)) {
        ZHLN::Log("ERROR: Failed to build 3D mesh particle update compute pipeline!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // 2. Pipeline Layout for 3D Mesh Particle Graphics Pipelines
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .AddDescriptorSetLayout(bindlessLayout.GetSetLayout())
        .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MeshParticleRenderPush))
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            meshParticleRenderLayout = std::forward<decltype(layout)>(layout);

            // 3. G-Buffer Deferred Graphics Pipeline (mesh_particle_render.hlsl)
            auto mpRenderShaders = Resource::GetShaderProgram(MeshParticleRender);
            return LoadAndCreateShaders(
                       {.path = Resource::Paths::MeshParticleRenderVS, .fallback = mpRenderShaders.vertex, .entryPoint = "VSMain"},
                       {.path = Resource::Paths::MeshParticleRenderPS, .fallback = mpRenderShaders.fragment, .entryPoint = "PSMain"}
            )
                .and_then([&](auto&& shaders) -> std::expected<void, Error> {
                    return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                        .Shaders(shaders)
                        .Layout(meshParticleRenderLayout.Get())
                        .ColorFormats(ActiveGBuffer::array) // Writes to SceneColor, Velocity, NormRough
                        .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                        .DepthTest(true)
                        .DepthWrite(true) // Solid 3D geometry writes depth
                        .CullBack()
                        .Build(ctx.Device())
                        .transform([&](auto&& pipeline) { meshParticleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
                });
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
                        .Layout(meshParticleRenderLayout.Get())
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
        .and_then([&]() { return InitCullingResources(); })
        .and_then([&]() { return InitBindless(); })
        .and_then([&]() { return InitLineBuffers(); })
        .and_then([&]() { return BuildLinePipeline(); })
        .and_then([&]() { return BuildHangGpuPipeline(); })
        .and_then([&]() { return BuildHiZPipeline(); })
        .and_then([&]() { return BuildProceduralBakePipeline(); })
        .and_then([&]() {
            return CompileShadowPipeline(
                       ctx.Device(), Resource::ShaderPair {.vertex = Resource::GetShaderProgram(Basic).vertex, .fragment = Resource::shadow_frag}
            )
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
            auto fvb_res = CreateDoubleBuffered(allocator, sizeof(GPUVolumetricVolume) * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
            if (fvb_res) {
                frames.fogVolumesBuffer = std::move(*fvb_res);
            }
        });
}

namespace {

std::expected<Vk::ExtensionResult, Error> GetPlatformInstanceExtensions(Window& window, ZHLN::ValidationMode validationMode) noexcept {
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
        .Debug(validationMode != ZHLN::ValidationMode::Off)
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

std::expected<Vk::ExtensionResult, Error> GetDeviceExtensions(VkPhysicalDevice physicalDevice, bool isHeadless) noexcept {
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
    const VkPushConstantRange*      pushConstants = nullptr,
    uint32_t                        pushCount     = 0,
    bool                            additive      = false
) noexcept {
    return self->LoadAndCreateShaders(vs, ps).and_then([&](auto&& shaders) -> std::expected<void, Error> {
        if (pass.Build(self->ctx.Device(), shaders, colorFormats, pushConstants, pushCount, additive)) {
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
    const VkPushConstantRange*            pushConstants = nullptr,
    uint32_t                              pushCount     = 0,
    bool                                  additive      = false
) noexcept {
    return self->LoadAndCreateShaders(vs, ps).and_then([&](auto&& shaders) -> std::expected<void, Error> {
        if (pass.BuildVariants(self->ctx.Device(), shaders, colorFormats, pushConstants, pushCount, specInfos, additive)) {
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

    return GetPlatformInstanceExtensions(window, cfg.validationMode)
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
            // Adopt surface into RAII container (VK_NULL_HANDLE is safe for headless)
            impl->surface         = Vk::Surface(instance, raw_surface);
            HardwareCaps caps     = ProbeHardware(physicalInfo.handle, physicalInfo.properties.properties.apiVersion);
            auto         features = BuildFeatureChain(physicalInfo.handle, caps, cfg.validationMode);

            return GetDeviceExtensions(physicalInfo.handle, window.IsHeadless()).and_then([&](auto&& dev_exts) -> std::expected<void, Error> {
                return Vk::Context::Builder()
                    .Instance(instance)
                    .Surface(raw_surface)
                    .PhysicalDevice(physicalInfo)
                    .DeviceExtensions(dev_exts)
                    .DeviceFeatures(features.GetRoot())
                    .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                    .Build()
                    .transform([&](auto&& context) { impl->ctx = std::forward<decltype(context)>(context); });
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
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (res != VK_SUCCESS) {
            ZHLN::Log("ERROR: Failed to wait for idle on device destruction.");
        }
        _impl->stagingContext.reset();

        // --- SAFETY: Only shut down ImGui if it was actually initialized ---
        if (!_impl->window.IsTTY() && !_impl->window.IsHeadless()) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
    }
}

std::expected<void, Error> RenderContext::Impl::AllocateDynamicVertexBuffers(
    size_t maxVertices, DoubleBuffered<Vk::Buffer>& bufs, DoubleBuffered<VkDeviceAddress>& addrs, VkBufferUsageFlags extraFlags, const char* label
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
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .AddDescriptorSetLayout(bindlessLayout.GetSetLayout())
        .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ObjectConstants))
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            linePipelineLayout = std::forward<decltype(layout)>(layout);

            auto basicShaders = Resource::GetShaderProgram(Resource::ShaderID::Basic);

            return LoadAndCreateShaders(
                       {.path = Resource::Paths::BasicVS, .fallback = basicShaders.vertex, .entryPoint = "VSMain"},
                       {.path = Resource::Paths::ForwardPS, .fallback = Resource::forward_frag, .entryPoint = "PSForward"}
            )
                .and_then([&](auto&& shaders) -> std::expected<void, Error> {
                    return Vk::PipelineBuilder<1, true> {}
                        .Shaders(shaders)
                        .Layout(linePipelineLayout.Get())
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
        });
}

std::expected<void, Error> RenderContext::Impl::InitShadowResources() {
    using enum RenderInitError;

    return Vk::SamplerBuilder {}
        .Linear()
        .ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
        .DepthCompare()
        .Build(ctx.Device())
        .transform_error([](auto err) -> Error { return err; })

        // 1. Bind Sampler
        .and_then([&](auto&& sampler) -> std::expected<void, Error> {
            shadowSampler = std::forward<decltype(sampler)>(sampler);
            return {};
        })

        // 2. Allocate Cascaded Shadow Map Render Target
        .and_then([&]() -> std::expected<void, Error> {
            graphResources.shadowMap = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = NUM_CASCADES}
            );
            shadowMapPrev = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = NUM_CASCADES}
            );
            if (!graphResources.shadowMap.Valid() || !shadowMapPrev.Valid()) [[unlikely]] {
                return std::unexpected(SubsystemAllocationFailed);
            }
            return {};
        })

        // 3. Create Cascade Image Views
        .and_then([&]() -> std::expected<void, Error> {
            shadowCascadeViews.resize(NUM_CASCADES);
            shadowCascadeViewsPrev.resize(NUM_CASCADES);
            for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
                shadowCascadeViews[i]     = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowMap.image.Handle(), i, 1);
                shadowCascadeViewsPrev[i] = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), shadowMapPrev.image.Handle(), i, 1);
                if (!shadowCascadeViews[i].Valid() || !shadowCascadeViewsPrev[i].Valid()) [[unlikely]] {
                    return std::unexpected(SubsystemAllocationFailed);
                }
            }
            return {};
        })

        // 4. Allocate Punctual Shadow Atlas Render Target
        .and_then([&]() -> std::expected<void, Error> {
            graphResources.shadowAtlas = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = 1024, .height = 1024},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = 24}
            );
            if (!graphResources.shadowAtlas.Valid()) [[unlikely]] {
                return std::unexpected(SubsystemAllocationFailed);
            }
            return {};
        })

        // 5. Create Atlas Image Views
        .and_then([&]() -> std::expected<void, Error> {
            shadowAtlasCubeView = Vk::CreateViewCubeArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 24);
            shadowAtlas2DView   = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 0, 24);
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
        .and_then([&]() {
            return CreateDoubleBuffered(allocator, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
                .transform_error([](auto err) -> Error { return err; });
        })

        // 8. Allocate Double-Buffered Light Storage Buffers
        .and_then([&](auto&& fub) {
            frames.frameUniformBuffers = std::forward<decltype(fub)>(fub);
            return CreateDoubleBuffered(allocator, sizeof(GPULight) * 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
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

    // 1. Initial side-effects: reflect the culling layout out of the compiled
    //    shader, then allocate the per-frame descriptor sets.
    auto cullingShader = Vk::CreateShaderDesc(Resource::culling_comp);
    if (!cullingLayout.Build(ctx.Device(), cullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        ZHLN::Log("[RenderInit] ERROR: Failed to reflect culling layout from culling SPV!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    cullingPool = cullingLayout.CreatePool(ctx.Device(), 4);

    frames.cullingSetsPass1[0] = CullingLayout::Allocate(ctx.Device(), cullingPool.Get(), cullingLayout.GetSetLayout());
    frames.cullingSetsPass1[1] = CullingLayout::Allocate(ctx.Device(), cullingPool.Get(), cullingLayout.GetSetLayout());
    frames.cullingSetsPass2[0] = CullingLayout::Allocate(ctx.Device(), cullingPool.Get(), cullingLayout.GetSetLayout());
    frames.cullingSetsPass2[1] = CullingLayout::Allocate(ctx.Device(), cullingPool.Get(), cullingLayout.GetSetLayout());

    auto make_instance_set = [&](uint32_t i) -> std::expected<void, Error> {
        return Vk::Buffer::Create(
                   allocator.Get(), sizeof(InstanceData) * kGpuCullingMaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
        )
            .and_then([&, i](auto&& idb) {
                frames.instanceDataBuffers[i] = std::forward<decltype(idb)>(idb);
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .and_then([&, i](auto&& icb1) {
                frames.indirectCommandsBuffers[i] = std::forward<decltype(icb1)>(icb1);
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .and_then([&, i](auto&& icb2) {
                frames.indirectCommandsBuffersPass2[i] = std::forward<decltype(icb2)>(icb2);
                // NOTE: TRANSFER_SRC on both indirect buffers exists for the
                // ZHLN_DEBUG_INDIRECT readback (end-of-frame head copy).
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(uint32_t) * kGpuCullingMaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VMA_MEMORY_USAGE_GPU_ONLY
                );
            })
            .and_then([&, i](auto&& spcb) {
                frames.secondPassCandidatesBuffers[i] = std::forward<decltype(spcb)>(spcb);
                return Vk::Buffer::Create(
                    allocator.Get(), sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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

    VkPushConstantRange cullingPush = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(CullingConstants),
    };

    return make_instance_set(0)
        .and_then([&]() { return make_instance_set(1); })
        .and_then([&]() { return cullingPass.Build(ctx.Device(), cullingLayout.GetSetLayout(), cullingShader, &cullingPush, 1); })
        .and_then([&]() -> std::expected<void, Error> {
            constexpr auto numClusters = static_cast<size_t>(16 * 9 * 24);

            return Vk::Buffer::Create(
                       allocator.Get(), sizeof(struct ClusterBounds) * numClusters, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY
            )
                .transform_error([](VkResult res) -> Error { return res; })
                .and_then([&, numClusters](auto&& cbb) -> std::expected<void, Error> {
                    clusterBoundsBuffer = std::forward<decltype(cbb)>(cbb);

                    // Reflect the cluster-culling layout out of the compiled shader, then
                    // allocate the double-buffered set pair.
                    auto ccShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterCulling).vertex);
                    if (!clusterCullingDescLayout.Build(ctx.Device(), ccShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
                        ZHLN::Log("[RenderInit] ERROR: Failed to reflect cluster-culling layout!");
                        return std::unexpected(RenderInitError::PipelineCreationFailed);
                    }
                    clusterCullingPool = clusterCullingDescLayout.CreatePool(ctx.Device(), 2);
                    frames.clusterCullingSets[0] =
                        clusterCullingDescLayout.Allocate(ctx.Device(), clusterCullingPool.Get(), clusterCullingDescLayout.GetSetLayout());
                    frames.clusterCullingSets[1] =
                        clusterCullingDescLayout.Allocate(ctx.Device(), clusterCullingPool.Get(), clusterCullingDescLayout.GetSetLayout());

                    auto make_cluster_set = [&](uint32_t i) -> std::expected<void, Error> {
                        return Vk::Buffer::Create(
                                   allocator.Get(), sizeof(ClusterVolume) * numClusters, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_MEMORY_USAGE_GPU_ONLY
                        )
                            .transform_error([](VkResult res) -> Error { return res; })
                            .and_then([&, i](auto&& cgb) {
                                frames.clusterGridBuffers[i] = std::forward<decltype(cgb)>(cgb);
                                return Vk::Buffer::Create(
                                           allocator.Get(), sizeof(uint32_t) * numClusters * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                                )
                                    .transform_error([](VkResult res) -> Error { return res; });
                            })
                            .and_then([&, i](auto&& lsb) {
                                frames.lightIndexListBuffers[i] = std::forward<decltype(lsb)>(lsb);
                                return Vk::Buffer::Create(
                                           allocator.Get(), sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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

                                // Order mirrors cluster_culling.slang's set-0 declaration order:
                                // in_Bounds, out_Grid, out_IndexList, out_Counter, frame, lights.
                                clusterCullingDescLayout.Write(
                                    ctx.Device(), frames.clusterCullingSets[i], Vk::BufferWrite {.buffer = clusterBoundsBuffer.Handle()},
                                    Vk::BufferWrite {.buffer = frames.clusterGridBuffers[i].Handle()},
                                    Vk::BufferWrite {.buffer = frames.lightIndexListBuffers[i].Handle()},
                                    Vk::BufferWrite {.buffer = frames.globalCounterBuffers[i].Handle()},
                                    Vk::BufferWrite {.buffer = frames.frameUniformBuffers[i].Handle()},
                                    Vk::BufferWrite {.buffer = frames.lightStorageBuffers[i].Handle()}
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
            clusterBoundsPool = clusterBoundsDescLayout.CreatePool(ctx.Device(), 2);
            for (int i = 0; i < 2; ++i) {
                frames.clusterBoundsSets[i] = clusterBoundsDescLayout.Allocate(ctx.Device(), clusterBoundsPool.Get(), clusterBoundsDescLayout.GetSetLayout());
                // Order mirrors cluster_bounds.slang's set-0 declaration order: out_Bounds, frame.
                clusterBoundsDescLayout.Write(
                    ctx.Device(), frames.clusterBoundsSets[i], Vk::BufferWrite {.buffer = clusterBoundsBuffer.Handle()},
                    Vk::BufferWrite {.buffer = frames.frameUniformBuffers[i].Handle()}
                );
            }
            return clusterBoundsPass.Build(ctx.Device(), clusterBoundsDescLayout.GetSetLayout(), bDesc);
        })
        .and_then([&]() {
            auto cDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterCulling).vertex);
            return clusterCullingPass.Build(ctx.Device(), clusterCullingDescLayout.GetSetLayout(), cDesc);
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
    // members in live use: {0,1,2,3,4,5,6,10,11}, with the runtime texture
    // array (11) picking up partially-bound / update-after-bind flags.
    auto basicShaders = Resource::GetShaderProgram(Basic);
    return LoadAndCreateShaders(
               {.path = Resource::Paths::BasicVS, .fallback = basicShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::BasicPS, .fallback = basicShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& basicStages) -> std::expected<Vk::Sampler, Error> {
            const Vk::ReflectedStageInput reflectInputs[6] = {
                {.shader = Vk::CreateShaderDesc(basicStages.GetVertSpv()), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc(basicStages.GetFragSpv()), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                {.shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(PunctualShadows).vertex), .stage = VK_SHADER_STAGE_VERTEX_BIT},
                {.shader = Vk::CreateShaderDesc(Resource::forward_frag), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
                // Compute consumers widen the stage flags of the members they
                // touch (`scene.frame` for both particle simulations). Without
                // them the union layout carries only VS|FS stage flags and
                // vkCreateComputePipelines trips VUID-07988.
                {.shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ParticleUpdate).vertex), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
                {.shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(MeshParticleUpdate).vertex), .stage = VK_SHADER_STAGE_COMPUTE_BIT},
            };
            if (!bindlessLayout.Build(ctx.Device(), std::span {reflectInputs})) {
                ZHLN::Log("[RenderInit] ERROR: Failed to reflect the global bindless layout!");
                return std::unexpected(RenderInitError::PipelineCreationFailed);
            }

            bindlessPool           = bindlessLayout.CreatePool(ctx.Device(), 2);
            frames.bindlessSets[0] = bindlessLayout.Allocate(ctx.Device(), bindlessPool.Get(), bindlessLayout.GetSetLayout());
            frames.bindlessSets[1] = bindlessLayout.Allocate(ctx.Device(), bindlessPool.Get(), bindlessLayout.GetSetLayout());

            return Vk::SamplerBuilder {}
                .Linear()
                .Repeat()
                .Anisotropy(ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy)
                .LodRange(0.0f, 0.0f)
                .Build(ctx.Device())
                .transform_error([](auto err) -> Error { return err; });
        })
        .and_then([&](auto&& globalRes) -> std::expected<void, Error> {
            globalSampler = std::forward<decltype(globalRes)>(globalRes);

            return Vk::SamplerBuilder {}
                .Linear()
                .ClampToEdge()
                .Build(ctx.Device())
                .transform_error([](auto err) -> Error { return err; })
                .transform([&](auto&& clampRes) { clampSampler = std::forward<decltype(clampRes)>(clampRes); });
        })
        .and_then([&]() -> std::expected<void, Error> { return InitSkeletalAnimationResources(); })
        .and_then([&]() -> std::expected<void, Error> { return InitLightingLUTs(); })
        .and_then([&]() -> std::expected<void, Error> { return InitializeSystemTextures(); })
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

            // Update global descriptor bindings. The binding numbers mirror the
            // GlobalSceneRegistry member order in common.slang. Writes are gated
            // by HasBinding() because prefilteredMap/brdfLUT/clampSampler are
            // only declared (never sampled via `scene`) and are stripped from
            // the reflected layout by dead-code elimination.
            Vk::DescriptorUpdater bindlessRegistry;
            for (int i = 0; i < 2; ++i) {
                if (bindlessLayout.HasBinding(0, 0)) {
                    bindlessRegistry.BindSampler(0, globalSampler.Get());
                }
                bindlessRegistry.BindUniformBuffer(1, frames.frameUniformBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(2, frames.lightStorageBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(3, frames.instanceDataBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(4, frames.jointBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(5, frames.jointBuffers[1 - i].Handle());
                bindlessRegistry.BindStorageBuffer(6, morphDeltasBuffer.Handle());

                if (bindlessLayout.HasBinding(0, 7)) {
                    bindlessRegistry.BindSampledImage(7, iblPayload.prefilteredView.Get(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                if (bindlessLayout.HasBinding(0, 8)) {
                    bindlessRegistry.BindSampledImage(8, iblPayload.brdfLutView.Get(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                if (bindlessLayout.HasBinding(0, 9)) {
                    bindlessRegistry.BindSampler(9, clampSampler.Get());
                }
                // Binding 10 (texTransLighting) is written by RecreateTargets once
                // the translucent-lighting target exists. Binding 11 is the dynamic
                // globalTextures pool (UpdateBindlessTextureSlot).

                bindlessRegistry.UpdateSet(ctx.Device(), frames.bindlessSets[i]);
            }
            return {};
        });
}

std::expected<void, Error> RenderContext::Impl::BuildDecalPipeline() {
    using enum Resource::ShaderID;

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(DecalPushConstants) // 144 bytes
    };

    static constexpr std::array<VkFormat, 2> decalFormats = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM};

    auto decalShaders = Resource::GetShaderProgram(Decal);

    // Reflects decal.slang set 0 ({texDepth, pointSampler}) and set 1 (the scene
    // parameter block subset). The pipeline layout is assembled BY HAND below so
    // set 1 gets the FULL bindless layout handle (subset is compatible).
    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc(decalShaders.vertex), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc(decalShaders.fragment), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    if (!decalDescLayout.Build(ctx.Device(), std::span {reflectInputs})) {
        ZHLN::Log("[RenderInit] ERROR: Failed to reflect decal descriptor layout!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    decalDescPool = decalDescLayout.CreatePool(ctx.Device(), 1);
    decalSet      = decalDescLayout.Allocate(ctx.Device(), decalDescPool.Get(), decalDescLayout.GetSetLayout());

    return Vk::PipelineLayoutBuilder(ctx.Device())
        .AddDescriptorSetLayout(decalDescLayout.GetSetLayout()) // Set 0: Decal (texDepth, pointSampler)
        .AddDescriptorSetLayout(bindlessLayout.GetSetLayout())  // Set 1: Global Bindless Layout
        .AddPushConstant(push.stageFlags, push.size, push.offset)
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            decalPipelineLayout = std::forward<decltype(layout)>(layout);

            return LoadAndCreateShaders(
                       {.path = Resource::Paths::DecalVS, .fallback = decalShaders.vertex, .entryPoint = "VSMain"},
                       {.path = Resource::Paths::DecalPS, .fallback = decalShaders.fragment, .entryPoint = "PSMain"}
            )
                .and_then([&](auto&& shaders) -> std::expected<void, Error> {
                    return Vk::PipelineBuilder<2, true> {} // Updated from 3 to 2 attachments
                        .Shaders(shaders)
                        .Layout(decalPipelineLayout.Get())
                        .ColorFormats(decalFormats) // Explicit 2-format array
                        .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                        .DepthTest(true)
                        .DepthWrite(false)
                        .CullFront()
                        .AlphaBlend()
                        .Build(ctx.Device())
                        .transform([&](auto&& pipeline) { decalPipeline = std::forward<decltype(pipeline)>(pipeline); });
                });
        });
}

std::expected<void, Error> RenderContext::Impl::BuildTAAPipeline() {
    using enum Resource::ShaderID;
    VkPushConstantRange taaPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float)};

    return BuildPassHelper(
        this, taaPass, "TAA", {.path = Resource::Paths::TaaVS, .fallback = Resource::GetShaderProgram(Taa).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::TaaPS, .fallback = Resource::GetShaderProgram(Taa).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT},
        &taaPush, 1
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
    VkPushConstantRange smaaPush = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 4};

    return BuildPassHelper(
               this, smaaEdgePass, "SMAA Edge Detection", {.path = Resource::Paths::SmaaEdgeVS, .fallback = Resource::GetShaderProgram(SmaaEdge).vertex},
               {.path = Resource::Paths::SmaaEdgePS, .fallback = Resource::GetShaderProgram(SmaaEdge).fragment}, {VK_FORMAT_R8G8_UNORM}, &smaaPush, 1
    )
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaWeightPass, "SMAA Blending Weight",
                {.path = Resource::Paths::SmaaWeightVS, .fallback = Resource::GetShaderProgram(SmaaWeight).vertex},
                {.path = Resource::Paths::SmaaWeightPS, .fallback = Resource::GetShaderProgram(SmaaWeight).fragment}, {VK_FORMAT_R8G8B8A8_UNORM}, &smaaPush, 1
            );
        })
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaBlendPass, "SMAA Neighborhood Blend",
                {.path = Resource::Paths::SmaaBlendVS, .fallback = Resource::GetShaderProgram(SmaaBlend).vertex},
                {.path = Resource::Paths::SmaaBlendPS, .fallback = Resource::GetShaderProgram(SmaaBlend).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}, &smaaPush,
                1
            );
        });
}

std::expected<void, Error> RenderContext::Impl::BuildAmbientPipeline() {
    using enum Resource::ShaderID;
    VkPushConstantRange ppPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 192};

    return BuildPassHelper(
        this, ambientPass, "Ambient", {.path = Resource::Paths::AmbientVS, .fallback = Resource::GetShaderProgram(Ambient).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::AmbientPS, .fallback = Resource::GetShaderProgram(Ambient).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT},
        &ppPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildLightingPipeline() {
    using enum Resource::ShaderID;
    VkPushConstantRange ppPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(PPPushConstants)};

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
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, &ppPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildReflectionPipelines() {
    using enum Resource::ShaderID;
    VkPushConstantRange ppPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(PPPushConstants)};

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
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, &ppPush, 1
    );
    if (!res) {
        return res;
    }

    return BuildPassVariants(
        this, translucentReflectionPass, "Translucent Reflection", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, &ppPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildBloomPipelines() {
    using enum Resource::ShaderID;

    auto res = BuildPassHelper(
        this, bloomThresholdPass, "Bloom Threshold", {.path = Resource::Paths::BloomThresholdVS, .fallback = Resource::GetShaderProgram(BloomThreshold).vertex},
        {.path = Resource::Paths::BloomThresholdPS, .fallback = Resource::GetShaderProgram(BloomThreshold).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );

    VkPushConstantRange kawasePush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(KawasePushConstants)};

    for (int i = 0; i < 3; ++i) {
        res = res.and_then(
                     [&, i]() {
                         std::string downName = std::format("Bloom Downsample {}", i);
                         return BuildPassHelper(
                             this, bloomDownPass[i], downName.c_str(),
                             {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                             {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment},
                             {VK_FORMAT_R16G16B16A16_SFLOAT}, &kawasePush, 1
                         );
                     }
        ).and_then([&, i]() {
            std::string upName = std::format("Bloom Upsample {}", i);
            return BuildPassHelper(
                this, bloomUpPass[i], upName.c_str(), {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT},
                &kawasePush, 1
            );
        });
    }

    return res;
}

std::expected<void, Error> RenderContext::Impl::BuildBlitPipeline() {
    using enum Resource::ShaderID;
    VkPushConstantRange blitPush = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(BlitPushConstants),
    };

    return BuildPassHelper(
        this, blitPass, "Blit", {.path = Resource::Paths::BlitVS, .fallback = Resource::GetShaderProgram(Blit).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::BlitPS, .fallback = Resource::GetShaderProgram(Blit).fragment, .entryPoint = "PSMain"}, {presentation.GetPresentFormat()},
        &blitPush, 1
    );
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
        if (!volumetricClearPass.Build(ctx.Device(), csClear)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        VkPushConstantRange fogPush     = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(VolumetricFogInjectPushConstants)};
        auto                csFogInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricFogInject).vertex);
        if (!volumetricFogInjectPass.Build(ctx.Device(), csFogInject, &fogPush, 1)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        VkPushConstantRange lightPush     = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(VolumetricLightInjectPushConstants)};
        auto                csLightInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricLightInject).vertex);
        if (!volumetricLightInjectPass.Build(ctx.Device(), csLightInject, &lightPush, 1)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        auto csIntegrate = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricIntegration).vertex);
        if (!volumetricIntegrationPass.Build(ctx.Device(), csIntegrate)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        VkPushConstantRange tempPush   = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(VolumetricTemporalPushConstants)};
        auto                csTemporal = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricTemporal).vertex);
        if (!volumetricTemporalPass.Build(ctx.Device(), csTemporal, &tempPush, 1)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        return {};
    };

    return Vk::SamplerBuilder {}
        .Linear()
        .ClampToEdge()
        .Build(ctx.Device())
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&](auto defaultResult) -> std::expected<void, Error> {
            defaultSampler = std::move(defaultResult);
            return Vk::SamplerBuilder {}
                .Nearest()
                .ClampToEdge()
                .Build(ctx.Device())
                .transform_error([](auto err) -> Error { return err; })
                .transform([&](auto pointResult) { pointSampler = std::move(pointResult); });
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

            return Vk::PipelineLayoutBuilder(ctx.Device())
                .AddDescriptorSetLayout(bindlessLayout.GetSetLayout())
                .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ObjectConstants))
                .Build()
                .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; });
        })
        .and_then([&](auto&& layout) {
            csgPipelineLayout = std::forward<decltype(layout)>(layout);

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
                .Layout(csgPipelineLayout.Get())
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
                .Layout(csgPipelineLayout.Get())
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
                .Layout(csgPipelineLayout.Get())
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

    return Vk::DescriptorPoolBuilder(ctx.Device())
        .Flags(VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
        .MaxSets(1000)
        .AddSize(VK_DESCRIPTOR_TYPE_SAMPLER, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000)
        .AddSize(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000)
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
        .and_then([&](auto&& poolRes) -> std::expected<void, Error> {
            uiPool = std::forward<decltype(poolRes)>(poolRes);

            return Vk::ShaderStages::Create(ctx.Device(), Resource::GetShaderProgram(Ui))
                .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
                .transform([&](auto&& shaders) -> void { uiShaders = std::forward<decltype(shaders)>(shaders); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            return Vk::PipelineLayoutBuilder(ctx.Device())
                .AddDescriptorSetLayout(bindlessLayout.GetSetLayout())
                .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(UIObjectConstants))
                .Build()
                .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
                .transform([&](auto&& layout) { uiPipelineLayout = std::forward<decltype(layout)>(layout); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            VkFormat swapchainFormat = presentation.GetPresentFormat();

            return Vk::PipelineBuilder {}
                .Shaders(uiShaders)
                .Layout(uiPipelineLayout.Get())
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

                ImGui_ImplVulkan_InitInfo init_info = {
                    .ApiVersion         = VK_API_VERSION_1_3,
                    .Instance           = ctx.Instance(),
                    .PhysicalDevice     = ctx.Physical(),
                    .Device             = ctx.Device(),
                    .QueueFamily        = ctx.PhysicalInfo().graphics_family,
                    .Queue              = ctx.GraphicsQueue(),
                    .DescriptorPool     = uiPool.Get(),
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
                };

                return make_expected(ImGui_ImplVulkan_Init(&init_info), RenderInitError::UISetupFailed);
            }
            return {};
        });
}

void RenderContext::Impl::RecreatePunctualShadowViews() noexcept {
    punctualShadowViews.clear();
    punctualShadowViews.resize(MAX_PUNCTUAL_LIGHTS);
    for (uint32_t i = 0; i < MAX_PUNCTUAL_LIGHTS; ++i) {
        punctualShadowViews[i] = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(
            ctx.Device(), graphResources.shadowAtlas.image.Handle(),
            i * 6,                    // baseLayer
            6,                        // layerCount
            VK_IMAGE_ASPECT_DEPTH_BIT // aspect
        );
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

            ltcMatView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcMatImage.Handle());
            ltcAmpView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcAmpImage.Handle());

            ApplyImageDebugNames(*this);
        });
}

bool RenderContext::Impl::RecreateTargets(VkExtent2D ext) {
    if (!presentation.Rebuild(ext.width, ext.height)) {
        return false;
    }

    graphResources.sceneColor            = CreateDefaultTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>(ext);
    graphResources.velocityBuffer        = CreateDefaultTarget<VK_FORMAT_R16G16_SFLOAT>(ext);
    frames.accumBuffers[0]               = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    frames.accumBuffers[1]               = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    graphResources.normalRoughnessBuffer = CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext);
    graphResources.hdrSceneColor         = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    graphResources.ambientTarget         = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
    graphResources.lightingTarget        = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
    graphResources.smaaEdgeTarget        = CreateDefaultTarget<VK_FORMAT_R8G8_UNORM>(ext);
    graphResources.smaaWeightTarget      = CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext);

    VkExtent2D ext2  = {.width = std::max(1u, ext.width / 2), .height = std::max(1u, ext.height / 2)};
    VkExtent2D ext4  = {.width = std::max(1u, ext.width / 4), .height = std::max(1u, ext.height / 4)};
    VkExtent2D ext8  = {.width = std::max(1u, ext.width / 8), .height = std::max(1u, ext.height / 8)};
    VkExtent2D ext16 = {.width = std::max(1u, ext.width / 16), .height = std::max(1u, ext.height / 16)};
    // 160x90 aligns cleanly with 16x9 light clusters, maintaining 10x subdivision.
    VkExtent3D voxelExt = {.width = 160, .height = 90, .depth = 64};

    graphResources.voxelMedia = Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
        allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );

    graphResources.voxelLight = Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
        allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );

    graphResources.voxelIntegrated = Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
        allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );

    graphResources.voxelHistory = Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
        allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );

    graphResources.voxelResolved = Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
        allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );

    graphResources.bloomThresholdTarget = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2);
    graphResources.bloomDown1           = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4);
    graphResources.bloomDown2           = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8);
    graphResources.bloomDown3           = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext16);
    graphResources.bloomUp2             = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8);
    graphResources.bloomUp1             = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4);
    graphResources.bloomFinalTarget     = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2);
    graphResources.transNormalBuffer    = CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext);
    graphResources.transLightingTarget  = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
    graphResources.transDepthBuffer     = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
        allocator, ctx, ext, {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
    );
    graphResources.hizMap = Vk::MipmappedRenderTarget<VK_FORMAT_R32_SFLOAT>::Create(
        allocator, ctx, ext,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );

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

        if (decalSet != VK_NULL_HANDLE) {
            decalDescLayout.Write(ctx.Device(), decalSet, presentation.depthTarget.view.Get(), pointSampler.Get());
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
        // (The five voxel volumes above end their clear sequence in GENERAL,
        // matching every consumer: ComputeWrite/ComputeReadGeneral in the
        // compute graph, ShaderReadGeneral in the frame graph's Reflection
        // passes. The frame graph seeds resources it never writes as external
        // read-only inputs at their first-usage layout and emits no barrier
        // for them, so GENERAL must be their rest layout.)

        // The HiZ pyramid is the only render target not covered by the lists
        // above: without this warm-up its whole mip chain sits in UNDEFINED
        // until the first HiZGenerate, yet the two occlusion culling sets
        // sample the full-chain view (written READ_ONLY) before that — pass 1
        // already runs inside MainShadow, one pass earlier than HiZGenerate.
        // Clear it to far depth so frame-0 culling conservatively occludes
        // nothing instead of consuming raw VRAM garbage, and land it in its
        // per-frame steady state (MainPass2's ComputeRead leaves it
        // READ_ONLY) so every culling read is always well-defined.
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
    Vk::DescriptorUpdater bindlessRegistry;
    for (int i = 0; i < 2; ++i) {
        bindlessRegistry.BindSampledImage(10, graphResources.transLightingTarget.view.Get(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        bindlessRegistry.UpdateSet(ctx.Device(), frames.bindlessSets[i]);
        bindlessRegistry.Clear();
    }

    if (hizPool.Valid()) {
        hizPool = {};
    }
    uint32_t mips = graphResources.hizMap.mipLevels;
    hizPool       = hizDescLayout.CreatePool(ctx.Device(), mips);
    hizSets.resize(mips);

    for (uint32_t i = 0; i < mips; ++i) {
        hizSets[i] = HiZGenerateLayout::Allocate(ctx.Device(), hizPool.Get(), hizDescLayout.GetSetLayout());
        if (i == 0) {
            // hiz SPV keeps only {inDepth@0, outDepth@1}: the trailing sampler
            // arg is safely skipped by the reflected writer.
            hizDescLayout.Write(ctx.Device(), hizSets[i], presentation.depthTarget.view.Get(), graphResources.hizMap.mipViews[0].Get(), pointSampler.Get());
        } else {
            // The whole HiZ pyramid sits in VK_IMAGE_LAYOUT_GENERAL for the
            // duration of the pass (the graph declares it ComputeWrite), so
            // the sampled side must be written with GENERAL too — only the
            // base pass's depth input is a true READ_ONLY resource.
            hizDescLayout.Write(
                ctx.Device(), hizSets[i], Vk::ImageWrite {.view = graphResources.hizMap.mipViews[i - 1].Get(), .layout = VK_IMAGE_LAYOUT_GENERAL},
                Vk::ImageWrite {.view = graphResources.hizMap.mipViews[i].Get(), .layout = VK_IMAGE_LAYOUT_GENERAL}, pointSampler.Get()
            );
        }
    }

    for (int i = 0; i < 2; ++i) {
        // Pass 1 Set (Reads Previous Frame's Hi-Z)
        // Order mirrors culling.slang's set-0 declaration order.
        cullingLayout.Write(
            ctx.Device(), frames.cullingSetsPass1[i], Vk::BufferWrite {.buffer = frames.instanceDataBuffers[i].Handle()},
            Vk::BufferWrite {.buffer = frames.indirectCommandsBuffers[i].Handle()}, graphResources.hizMap.fullView.Get(), pointSampler.Get(),
            Vk::BufferWrite {.buffer = frames.secondPassCandidatesBuffers[i].Handle()}, Vk::BufferWrite {.buffer = frames.secondPassCountBuffers[i].Handle()}
        );

        // Pass 2 Set (Reads Current Frame's Hi-Z)
        cullingLayout.Write(
            ctx.Device(), frames.cullingSetsPass2[i], Vk::BufferWrite {.buffer = frames.instanceDataBuffers[i].Handle()},
            Vk::BufferWrite {.buffer = frames.indirectCommandsBuffersPass2[i].Handle()}, graphResources.hizMap.fullView.Get(), pointSampler.Get(),
            Vk::BufferWrite {.buffer = frames.secondPassCandidatesBuffers[i].Handle()}, Vk::BufferWrite {.buffer = frames.secondPassCountBuffers[i].Handle()}
        );
    }

    ApplyImageDebugNames(*this);

    return true;
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
    VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(float) * 2 + sizeof(uint32_t) * 3};
    return hizGeneratePass.Build(ctx.Device(), hizDescLayout.GetSetLayout(), shader, &pc, 1);
}

std::expected<void, Error> RenderContext::Impl::InitUIDynamicBuffers() noexcept {
    return AllocateDynamicVertexBuffers(kMaxUiVertices, frames.uiVbos, frames.uiVboAddresses, 0, "UI");
}

} // namespace ZHLN
