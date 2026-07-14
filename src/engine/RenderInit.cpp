// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderInit.cpp
#include "IBLProcessor.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "SMAALUTGenerator.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "engine/TTYBackend.hpp"
#include "imgui.h"
#include <Features.hpp>
#include <StagingContext.hpp>
#include <Zahlen/Error.hpp>
#include <cstddef>
#include <functional>
#include <stb_image.h>
#include <threading/TaskSystem.hpp>
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

std::expected<void, Error> RenderContext::Impl::InitSubsystems(const RenderConfig& cfg, int width, int height) {
    using enum Resource::ShaderID;

    auto make_expected = [](bool success, Error err) -> std::expected<void, Error> {
        if (success) {
            return {};
        }
        return std::unexpected(err);
    };

    return make_expected(allocator.Init(ctx), RenderInitError::SubsystemAllocationFailed)
        .and_then([&]() {
            return make_expected(
                stagingRingBuffer.Init(
                    allocator.Get(), ctx.Device(), ctx.GraphicsQueue(), ctx.PhysicalInfo().graphics_family, static_cast<VkDeviceSize>(64 * 1024 * 1024)
                ),
                RenderInitError::SubsystemAllocationFailed
            );
        })
        .and_then([&]() {
            return make_expected(
                transferRingBuffer.Init(
                    allocator.Get(), ctx.Device(), ctx.TransferQueue(), ctx.PhysicalInfo().transfer_family, static_cast<VkDeviceSize>(64 * 1024 * 1024)
                ),
                RenderInitError::SubsystemAllocationFailed
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            bool supportsRayTracing = CheckRayTracingSupport(ctx.Physical());
            if (!supportsRayTracing || !rtCtx.Init(ctx.Device())) {
                ZHLN::Log("WARNING: Raytracing context failed to initialize. RTR will be disabled.");
            } else {
                ZHLN::Log("Raytracing context initialized successfully.");
            }

            gpuProfiler.Init(ctx.Device(), ctx.Physical(), ctx.PhysicalInfo().graphics_family);
            graphicsCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().graphics_family);
            transferCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().transfer_family);

            return InitShadowResources();
        })
        .and_then([&]() { return InitCullingResources(); })
        .and_then([&]() { return InitBindless(); })
        .and_then([&]() { return BuildHangGpuPipeline(); })
        .and_then([&]() {
            return CompileShadowPipeline(
                       ctx.Device(), Resource::ShaderPair {.vertex = Resource::GetShaderProgram(Basic).vertex, .fragment = Resource::shadow_frag}
            )
                .transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&]() {
            return CompilePunctualShadowPipeline(ctx.Device(), Resource::GetShaderProgram(PunctualShadows)).transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&]() {
            return make_expected(presentation.Init(ctx, allocator, surface.Get(), width, height, cfg.vsync), RenderInitError::PresentationFailed);
        })
        .and_then([&]() {
            sync  = Vk::FrameSync<2>::Create(ctx.Device());
            pools = Vk::CommandPools<2>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1});

            InitializeSystemTextures();

            return InitPostProcessing();
        })
        .and_then([&]() {
            auto* windowHandle = window.IsTTY() ? nullptr : static_cast<GLFWwindow*>(window.GetNativeHandle());
            return SetupUI(windowHandle);
        })
        .and_then([&]() -> std::expected<void, Error> {
            uint32_t workerCount = TaskSystem::GetWorkerCount() + 1;
            if (workerCount == 0) {
                workerCount = 1;
            }
            workerCmds.resize(workerCount);

            for (auto& worker: workerCmds) {
                for (auto& pool: worker.pools) {
                    pool = Vk::CommandPool<Vk::QueueType::Graphics>(ctx.Device(), ctx.PhysicalInfo().graphics_family);
                    if (!pool.AllocateSecondary(256)) {
                        return std::unexpected(RenderInitError::WorkerCommandPoolSetupFailed);
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
        .transform([&]() { deletionQueue.Init(2); });
}

namespace {

std::expected<Vk::ExtensionResult, Error> GetPlatformInstanceExtensions(Window& window, bool enableValidation) noexcept {
    glfwSetErrorCallback([](int error, const char* description) { ZHLN::Log("[GLFW Error] Code {}: {}", error, description); });

    auto builder = Vk::ExtensionBuilder::ForInstance();

    if (window.IsTTY()) {
        for (const auto ext: TTYBackend::GetRequiredInstanceExtensions()) {
            builder.Require(ext);
        }
    } else {
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

    return std::move(builder).Debug(enableValidation).OptionalIf("VK_KHR_portability_enumeration", isMac).Build().transform_error([](auto err) -> Error {
        return err;
    });
}

auto BuildFeatureChain(VkPhysicalDevice physicalDevice, const HardwareCaps& caps) noexcept {
    return Vk::FeatureChainBuilder(physicalDevice)
        .Require<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>([](auto& f) { f.swapchainMaintenance1 = VK_TRUE; })
        .Require<VkPhysicalDeviceVulkan11Features>([](auto& f) {
            f.multiview                          = VK_TRUE;
            f.storageBuffer16BitAccess           = VK_TRUE;
            f.uniformAndStorageBuffer16BitAccess = VK_TRUE;
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
        })
        .Optional<VkPhysicalDeviceAccelerationStructureFeaturesKHR>([](auto& f) { f.accelerationStructure = VK_TRUE; })
        .Optional<VkPhysicalDeviceRayQueryFeaturesKHR>([](auto& f) { f.rayQuery = VK_TRUE; })
        .Require<VkPhysicalDeviceFeatures2>([&](auto& f) {
            f.features.multiDrawIndirect         = VK_TRUE;
            f.features.samplerAnisotropy         = VK_TRUE;
            f.features.drawIndirectFirstInstance = VK_TRUE;
            f.features.shaderInt64               = caps.supportsInt64 ? VK_TRUE : VK_FALSE;
            f.features.imageCubeArray            = VK_TRUE;
        })
        .Build();
}

std::expected<Vk::ExtensionResult, Error> GetDeviceExtensions(VkPhysicalDevice physicalDevice) noexcept {
    return Vk::ExtensionBuilder::ForDevice(physicalDevice)
        .Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
        .Require(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)
        .Require(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME)
        .Require("VK_EXT_robustness2")
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
    ShaderStageSource               vs,
    ShaderStageSource               ps,
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
    ShaderStageSource                     vs,
    ShaderStageSource                     ps,
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

std::expected<Vk::ShaderStages, Error> RenderContext::Impl::LoadAndCreateShaders(ShaderStageSource vs, ShaderStageSource ps) const noexcept {
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

std::expected<Vk::Pipeline, Error> RenderContext::Impl::LoadAndCreateComputeShader(ShaderStageSource cs, VkPipelineLayout layout) const noexcept {
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

std::expected<std::unique_ptr<RenderContext>, Error> RenderContext::Create(Window& window, const RenderConfig& cfg) noexcept {
    auto impl     = std::make_unique<Impl>(window);
    impl->appName = cfg.appName;

    // Local state captured by reference and mutated synchronously inside the pipeline
    VkInstance              instance    = VK_NULL_HANDLE;
    VkSurfaceKHR            raw_surface = VK_NULL_HANDLE;
    int                     width       = 0;
    int                     height      = 0;
    ZHLN_PhysicalDeviceInfo physicalInfo {};

    auto make_expected = [](bool success, Error err) -> std::expected<void, Error> { return success ? std::expected<void, Error> {} : std::unexpected(err); };

    return GetPlatformInstanceExtensions(window, cfg.enableValidation)
        .and_then([&](auto&& inst_exts) -> std::expected<void, Error> {
            instance = Vk::Context::Builder().AppName(impl->appName).EnableValidation(cfg.enableValidation).InstanceExtensions(inst_exts).BuildInstance();
            return make_expected(instance != VK_NULL_HANDLE, RenderInitError::InstanceCreationFailed);
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (!window.IsTTY()) {
                return window.CreateVulkanSurface(instance, nullptr, width, height)
                    .transform_error([](auto) -> Error { return RenderInitError::SurfaceCreationFailed; })
                    .transform([&](void* surface) { raw_surface = static_cast<VkSurfaceKHR>(surface); });
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            physicalInfo = Vk::Context::Builder().Instance(instance).Surface(raw_surface).SelectPhysicalDevice();
            return make_expected(physicalInfo.handle != VK_NULL_HANDLE, RenderInitError::NoSuitableDeviceFound);
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
            auto         features = BuildFeatureChain(physicalInfo.handle, caps);

            return GetDeviceExtensions(physicalInfo.handle).and_then([&](auto&& dev_exts) -> std::expected<void, Error> {
                return Vk::Context::Builder()
                    .Instance(instance)
                    .Surface(raw_surface)
                    .PhysicalDevice(physicalInfo)
                    .DeviceExtensions(dev_exts)
                    .DeviceFeatures(features.GetRoot())
                    .EnableValidation(cfg.enableValidation)
                    .Build()
                    .transform([&](auto&& context) { impl->ctx = std::forward<decltype(context)>(context); });
            });
        })
        .and_then([&]() { return make_expected(impl->ctx.Valid(), RenderInitError::DeviceCreationFailed); })
        .and_then([&]() { return impl->InitSubsystems(cfg, width, height); })
        .transform([&]() { return std::make_unique<RenderContext>(PrivateToken {}, std::move(impl)); });
}

std::expected<void, Error> RenderContext::Impl::BuildSkinningPipeline() {
    VkPushConstantRange pushRange = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(SkinningConstants)};

    ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = nullptr, .set_layout_count = 0, .push_constants = &pushRange, .push_constant_count = 1};

    auto make_expected = [](bool success, Error err) -> std::expected<void, Error> {
        if (success) {
            return {};
        }
        return std::unexpected(err);
    };

    auto* rawLayout = ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc);

    return make_expected(rawLayout != VK_NULL_HANDLE, RenderInitError::PipelineLayoutCreationFailed).and_then([&]() -> std::expected<void, Error> {
        skinningPass.pipelineLayout = Vk::PipelineLayout(ctx.Device(), rawLayout);

        return LoadAndCreateComputeShader(
                   {.path = SHADER_SKINNING_HLSL_CS_PATH, .fallback = Resource::skinning_comp, .entryPoint = "CSMain"}, skinningPass.pipelineLayout.Get()
        )
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
        if (!_impl->window.IsTTY()) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
    }
}

std::expected<void, Error> RenderContext::Impl::InitShadowResources() {
    return Vk::SamplerBuilder {}
        .Linear()
        .ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
        .DepthCompare()
        .Build(ctx.Device())
        .transform_error([](auto err) -> Error { return err; })
        .transform([&](auto&& sampler) {
            shadowSampler = std::forward<decltype(sampler)>(sampler);

            graphResources.shadowMap = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = NUM_CASCADES}
            );

            shadowCascadeViews.resize(NUM_CASCADES);
            for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
                shadowCascadeViews[i] = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowMap.image.Handle(), i, 1);
            }

            // 1. Allocate Shadow Atlas
            graphResources.shadowAtlas = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = 1024, .height = 1024},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = 24}
            );

            // 2. Pre-allocate Views using clean C++ helper templates!
            shadowAtlasCubeView = Vk::CreateViewCubeArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 24);
            shadowAtlas2DView   = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 0, 24);

            Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
            });

            RecreatePunctualShadowViews();

            frameUniformBuffers = CreateDoubleBuffered(allocator, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

            lightStorageBuffers = CreateDoubleBuffered(allocator, sizeof(GPULight) * 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

            shadowIndirectBuffers = CreateDoubleBuffered(
                allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances * 8, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
            );
        });
}

std::expected<void, Error> RenderContext::Impl::InitCullingResources() {
    using enum Resource::ShaderID;

    // 1. Initial side-effects: Descriptor sets & Buffer allocation
    Vk::AllocateDoubleBufferedSet<CullingLayout>(ctx.Device(), cullingLayout, cullingPool, cullingSets);

    for (int i = 0; i < 2; ++i) {
        instanceDataBuffers[i] = Vk::Buffer::Create(
            allocator.Get(), sizeof(InstanceData) * kGpuCullingMaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
        );

        indirectCommandsBuffers[i] = Vk::Buffer::Create(
            allocator.Get(), sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY
        );

        CullingLayout::Write(
            ctx.Device(), cullingSets[i], Vk::BufferWrite {.buffer = instanceDataBuffers[i].Handle()},
            Vk::BufferWrite {.buffer = indirectCommandsBuffers[i].Handle()}
        );
    }

    constexpr uint32_t  kCullingPushSize = sizeof(float) * 4 * 6 + sizeof(uint32_t) * 4;
    VkPushConstantRange cullingPush      = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = kCullingPushSize,
    };

    auto cullingShader = Vk::CreateShaderDesc(Resource::culling_comp);

    // 2. Start the monadic pipeline with the Compute Culling build
    return cullingPass.Build(ctx.Device(), cullingLayout.Get(), cullingShader, &cullingPush, 1)
        .and_then([&]() -> std::expected<void, Error> {
            constexpr auto numClusters = static_cast<size_t>(16 * 9 * 24);

            clusterBoundsBuffer = Vk::Buffer::Create(
                allocator.Get(), sizeof(struct ClusterBounds) * numClusters, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VMA_MEMORY_USAGE_GPU_ONLY
            );

            Vk::AllocateDoubleBufferedSet<ClusterCullingLayout>(ctx.Device(), clusterCullingDescLayout, clusterCullingPool, clusterCullingSets);

            for (int i = 0; i < 2; ++i) {
                clusterGridBuffers[i] =
                    Vk::Buffer::Create(allocator.Get(), sizeof(ClusterVolume) * numClusters, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
                lightIndexListBuffers[i] =
                    Vk::Buffer::Create(allocator.Get(), sizeof(uint32_t) * numClusters * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
                globalCounterBuffers[i] = Vk::Buffer::Create(
                    allocator.Get(), sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                );

                ClusterCullingLayout::Write(
                    ctx.Device(), clusterCullingSets[i], Vk::BufferWrite {.buffer = clusterBoundsBuffer.Handle()},
                    Vk::BufferWrite {.buffer = clusterGridBuffers[i].Handle()}, Vk::BufferWrite {.buffer = lightIndexListBuffers[i].Handle()},
                    Vk::BufferWrite {.buffer = globalCounterBuffers[i].Handle()}, Vk::BufferWrite {.buffer = frameUniformBuffers[i].Handle()},
                    Vk::BufferWrite {.buffer = lightStorageBuffers[i].Handle()}
                );
            }

            auto bDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterBounds).vertex);
            return clusterBoundsPass.Build(ctx.Device(), clusterCullingDescLayout.Get(), bDesc);
        })
        .and_then([&]() {
            auto cDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterCulling).vertex);
            return clusterCullingPass.Build(ctx.Device(), clusterCullingDescLayout.Get(), cDesc);
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (rtCtx.Valid()) {
                ZHLN_AccelerationStructureSizes tlasSizes;
                rtCtx.GetTlasSizes(kGpuCullingMaxInstances, tlasSizes);

                for (int i = 0; i < 2; ++i) {
                    tlasBuffer[i] = Vk::Buffer::Create(
                        allocator.Get(), tlasSizes.acceleration_structure_size,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                    );

                    tlasScratchBuffer[i] = Vk::Buffer::Create(
                        allocator.Get(), tlasSizes.build_scratch_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY
                    );

                    tlas[i] = rtCtx.CreateAS(tlasBuffer[i].Handle(), tlasSizes.acceleration_structure_size, ZHLN_AS_TYPE_TOP_LEVEL);

                    tlasInstanceBuffers[i] = Vk::Buffer::Create(
                        allocator.Get(), sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY
                    );

                    tlasStagingBuffers[i] = Vk::Buffer::Create(
                        allocator.Get(), sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_CPU_ONLY
                    );
                }
            }

            return BuildSkinningPipeline();
        })
        .transform([&]() {
            if constexpr (isDev) {
                RegisterShaderWatcher(SHADER_SKINNING_HLSL_CS_PATH, [this]() {
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
    Vk::AllocateDoubleBufferedSet<GlobalSceneLayout>(ctx.Device(), bindlessLayout, bindlessPool, bindlessSets);

    // 1. Build and bind global and clamping samplers monadically
    return Vk::SamplerBuilder {}
        .Linear()
        .Repeat()
        .Anisotropy(ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy)
        .LodRange(0.0f, 0.0f)
        .Build(ctx.Device())
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&](auto&& globalRes) -> std::expected<void, Error> {
            globalSampler = std::forward<decltype(globalRes)>(globalRes);

            return Vk::SamplerBuilder {}
                .Linear()
                .ClampToEdge()
                .Build(ctx.Device())
                .transform_error([](auto err) -> Error { return err; })
                .and_then([&](auto&& clampRes) -> std::expected<void, Error> {
                    clampSampler = std::forward<decltype(clampRes)>(clampRes);
                    return {};
                });
        })
        // 2. Consolidate remaining void initializations into a terminal transform mapping
        .transform([&]() {
            InitSkeletalAnimationResources();
            InitLightingLUTs();

            ZHLN::Log("[RenderInit] Pre-allocating persistently mapped Double-Buffered Debug VBOs...");
            size_t maxDebugVerts = 500000;
            size_t bufferSize    = maxDebugVerts * (sizeof(VertexPosition) + sizeof(VertexAttributes));
            for (int i = 0; i < 2; ++i) {
                auto gpu_buf = Vk::Buffer::Create(
                    allocator.Get(), bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
                );

                auto address        = Vk::GetBufferAddress(ctx.Device(), gpu_buf.Handle());
                debugMeshHandles[i] = meshPool.Create(std::move(gpu_buf), maxDebugVerts, address);
            }

            // Update global descriptor bindings
            Vk::DescriptorUpdater bindlessRegistry;
            for (int i = 0; i < 2; ++i) {
                bindlessRegistry.BindSampler(1, globalSampler.Get());
                bindlessRegistry.BindUniformBuffer(2, frameUniformBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(3, lightStorageBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(4, instanceDataBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(5, jointBuffers[i].Handle());
                bindlessRegistry.BindStorageBuffer(6, jointBuffers[1 - i].Handle());
                bindlessRegistry.BindStorageBuffer(7, morphDeltasBuffer.Handle());

                bindlessRegistry.BindSampledImage(8, iblPayload.prefilteredView.Get(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                bindlessRegistry.BindSampledImage(9, iblPayload.brdfLutView.Get(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                bindlessRegistry.BindSampler(10, clampSampler.Get());

                bindlessRegistry.UpdateSet(ctx.Device(), bindlessSets[i]);
            }
        });
}

using enum Resource::ShaderID;
std::expected<void, Error> RenderContext::Impl::BuildTAAPipeline() {
    VkPushConstantRange taaPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float)};

    return BuildPassHelper(
        this, taaPass, "TAA", {.path = SHADER_TAA_HLSL_VS_PATH, .fallback = Resource::GetShaderProgram(Taa).vertex, .entryPoint = "VSMain"},
        {.path = SHADER_TAA_HLSL_PS_PATH, .fallback = Resource::GetShaderProgram(Taa).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT},
        &taaPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildFXAAPipeline() {
    VkPushConstantRange fxaaPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 6};

    return BuildPassHelper(
        this, fxaaPass, "FXAA", {.path = SHADER_FXAA_HLSL_VS_PATH, .fallback = Resource::GetShaderProgram(Fxaa).vertex, .entryPoint = "VSMain"},
        {.path = SHADER_FXAA_HLSL_PS_PATH, .fallback = Resource::GetShaderProgram(Fxaa).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT},
        &fxaaPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildSMAAPipeline() {
    VkPushConstantRange smaaPush = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 4};

    return BuildPassHelper(
               this, smaaEdgePass, "SMAA Edge Detection",
               {.path = SHADER_SMAA_EDGE_VS_PATH, .fallback = Resource::GetShaderProgram(SmaaEdge).vertex, .entryPoint = "SmaaEdgeVS"},
               {.path = SHADER_SMAA_EDGE_PS_PATH, .fallback = Resource::GetShaderProgram(SmaaEdge).fragment, .entryPoint = "SmaaEdgePS"},
               {VK_FORMAT_R8G8_UNORM}, &smaaPush, 1
    )
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaWeightPass, "SMAA Blending Weight",
                {.path = SHADER_SMAA_WEIGHT_VS_PATH, .fallback = Resource::GetShaderProgram(SmaaWeight).vertex, .entryPoint = "SmaaWeightVS"},
                {.path = SHADER_SMAA_WEIGHT_PS_PATH, .fallback = Resource::GetShaderProgram(SmaaWeight).fragment, .entryPoint = "SmaaWeightPS"},
                {VK_FORMAT_R8G8B8A8_UNORM}, &smaaPush, 1
            );
        })
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaBlendPass, "SMAA Neighborhood Blend",
                {.path = SHADER_SMAA_BLEND_VS_PATH, .fallback = Resource::GetShaderProgram(SmaaBlend).vertex, .entryPoint = "SmaaBlendVS"},
                {.path = SHADER_SMAA_BLEND_PS_PATH, .fallback = Resource::GetShaderProgram(SmaaBlend).fragment, .entryPoint = "SmaaBlendPS"},
                {VK_FORMAT_R16G16B16A16_SFLOAT}, &smaaPush, 1
            );
        });
}

std::expected<void, Error> RenderContext::Impl::BuildAmbientPipeline() {
    VkPushConstantRange ppPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 192};

    return BuildPassHelper(
        this, ambientPass, "Ambient", {.path = SHADER_AMBIENT_HLSL_VS_PATH, .fallback = Resource::GetShaderProgram(Ambient).vertex, .entryPoint = "VSMain"},
        {.path = SHADER_AMBIENT_HLSL_PS_PATH, .fallback = Resource::GetShaderProgram(Ambient).fragment, .entryPoint = "PSMain"},
        {VK_FORMAT_R16G16B16A16_SFLOAT}, &ppPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildLightingPipeline() {
    VkPushConstantRange ppPush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 192};

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
    const char* vsPath = hasRt ? SHADER_LIGHTING_HLSL_VS_PATH : SHADER_LIGHTING_NORT_HLSL_VS_PATH;
    const char* psPath = hasRt ? SHADER_LIGHTING_HLSL_PS_PATH : SHADER_LIGHTING_NORT_HLSL_PS_PATH;

    auto vsSpan = hasRt ? Resource::GetShaderProgram(Lighting).vertex : Resource::GetShaderProgram(LightingNort).vertex;
    auto psSpan = hasRt ? Resource::GetShaderProgram(Lighting).fragment : Resource::GetShaderProgram(LightingNort).fragment;

    return BuildPassVariants(
        this, lightingPass, "Lighting", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, &ppPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildReflectionPipelines() {
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
    const char* vsPath = hasRt ? SHADER_REFLECTION_HLSL_VS_PATH : SHADER_REFLECTION_NORT_HLSL_VS_PATH;
    const char* psPath = hasRt ? SHADER_REFLECTION_HLSL_PS_PATH : SHADER_REFLECTION_NORT_HLSL_PS_PATH;

    auto vsSpan = hasRt ? Resource::GetShaderProgram(Reflection).vertex : Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).vertex;
    auto psSpan = hasRt ? Resource::GetShaderProgram(Reflection).fragment : Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).fragment;

    return BuildPassVariants(
        this, reflectionPass, "Reflection", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, &ppPush, 1
    );
}

std::expected<void, Error> RenderContext::Impl::BuildBloomPipelines() {
    using enum Resource::ShaderID;

    auto res = BuildPassHelper(
        this, bloomThresholdPass, "Bloom Threshold",
        {.path = SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH, .fallback = Resource::GetShaderProgram(BloomThreshold).vertex, .entryPoint = "VSMain"},
        {.path = SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH, .fallback = Resource::GetShaderProgram(BloomThreshold).fragment, .entryPoint = "PSMain"},
        {VK_FORMAT_R16G16B16A16_SFLOAT}
    );

    VkPushConstantRange kawasePush = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(KawasePushConstants)};

    for (int i = 0; i < 3; ++i) {
        res = res.and_then(
                     [&, i]() {
                         std::string downName = std::format("Bloom Downsample {}", i);
                         return BuildPassHelper(
                             this, bloomDownPass[i], downName.c_str(),
                             {.path = SHADER_BLOOM_BLUR_HLSL_VS_PATH, .fallback = Resource::GetShaderProgram(BloomBlur).vertex, .entryPoint = "VSMain"},
                             {.path = SHADER_BLOOM_BLUR_HLSL_PS_PATH, .fallback = Resource::GetShaderProgram(BloomBlur).fragment, .entryPoint = "PSMain"},
                             {VK_FORMAT_R16G16B16A16_SFLOAT}, &kawasePush, 1
                         );
                     }
        ).and_then([&, i]() {
            std::string upName = std::format("Bloom Upsample {}", i);
            return BuildPassHelper(
                this, bloomUpPass[i], upName.c_str(),
                {.path = SHADER_BLOOM_BLUR_HLSL_VS_PATH, .fallback = Resource::GetShaderProgram(BloomBlur).vertex, .entryPoint = "VSMain"},
                {.path = SHADER_BLOOM_BLUR_HLSL_PS_PATH, .fallback = Resource::GetShaderProgram(BloomBlur).fragment, .entryPoint = "PSMain"},
                {VK_FORMAT_R16G16B16A16_SFLOAT}, &kawasePush, 1
            );
        });
    }

    return res;
}

std::expected<void, Error> RenderContext::Impl::BuildBlitPipeline() {
    VkPushConstantRange blitPush = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(BlitPushConstants),
    };

    return BuildPassHelper(
        this, blitPass, "Blit", {.path = SHADER_BLIT_HLSL_VS_PATH, .fallback = Resource::GetShaderProgram(Blit).vertex, .entryPoint = "VSMain"},
        {.path = SHADER_BLIT_HLSL_PS_PATH, .fallback = Resource::GetShaderProgram(Blit).fragment, .entryPoint = "PSMain"},
        {presentation.swapchain.Get().format}, &blitPush, 1
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
        auto csInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricInjection).vertex);
        if (!volumetricInjectionPass.Build(ctx.Device(), csInject)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        auto csScatter = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricScattering).vertex);
        if (!volumetricScatteringPass.Build(ctx.Device(), csScatter)) {
            return std::unexpected(RenderInitError::PipelineCreationFailed);
        }

        auto csIntegrate = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricIntegration).vertex);
        if (!volumetricIntegrationPass.Build(ctx.Device(), csIntegrate)) {
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
        .and_then([&]() { return register_and_check("TAA", [this]() { return BuildTAAPipeline(); }, {SHADER_TAA_HLSL_VS_PATH, SHADER_TAA_HLSL_PS_PATH}); })
        .and_then([&]() { return register_and_check("FXAA", [this]() { return BuildFXAAPipeline(); }, {SHADER_FXAA_HLSL_VS_PATH, SHADER_FXAA_HLSL_PS_PATH}); })
        .and_then([&]() {
            return register_and_check(
                "SMAA", [this]() { return BuildSMAAPipeline(); },
                {SHADER_SMAA_EDGE_VS_PATH, SHADER_SMAA_EDGE_PS_PATH, SHADER_SMAA_WEIGHT_VS_PATH, SHADER_SMAA_WEIGHT_PS_PATH, SHADER_SMAA_BLEND_VS_PATH,
                 SHADER_SMAA_BLEND_PS_PATH}
            );
        })
        .and_then([&]() {
            return register_and_check("Ambient", [this]() { return BuildAmbientPipeline(); }, {SHADER_AMBIENT_HLSL_VS_PATH, SHADER_AMBIENT_HLSL_PS_PATH});
        })
        .and_then([&]() {
            return register_and_check(
                "Lighting", [this]() { return BuildLightingPipeline(); },
                {SHADER_LIGHTING_HLSL_VS_PATH, SHADER_LIGHTING_HLSL_PS_PATH, SHADER_LIGHTING_NORT_HLSL_VS_PATH, SHADER_LIGHTING_NORT_HLSL_PS_PATH}
            );
        })
        .and_then([&]() {
            return register_and_check(
                "Reflection", [this]() { return BuildReflectionPipelines(); },
                {SHADER_REFLECTION_HLSL_VS_PATH, SHADER_REFLECTION_HLSL_PS_PATH, SHADER_REFLECTION_NORT_HLSL_VS_PATH, SHADER_REFLECTION_NORT_HLSL_PS_PATH}
            );
        })
        .and_then([&]() {
            return register_and_check(
                "Bloom", [this]() { return BuildBloomPipelines(); },
                {SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH, SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH, SHADER_BLOOM_BLUR_HLSL_VS_PATH, SHADER_BLOOM_BLUR_HLSL_PS_PATH}
            );
        })
        .and_then([&]() {
            return register_and_check(
                "Volumetrics", buildVolumetrics,
                {SHADER_VOLUMETRIC_INJECTION_CS_PATH, SHADER_VOLUMETRIC_SCATTERING_CS_PATH, SHADER_VOLUMETRIC_INTEGRATION_CS_PATH}
            );
        })
        .and_then([&]() { return register_and_check("Blit", [this]() { return BuildBlitPipeline(); }, {SHADER_BLIT_HLSL_VS_PATH, SHADER_BLIT_HLSL_PS_PATH}); })
        .transform([&]() {
            ZHLN::Array<uint32_t> smaaAreaPixels(static_cast<size_t>(160 * 560));
            ZHLN::PBR::FillSmaaAreaTex(smaaAreaPixels);
            ZHLN::Array<uint32_t> smaaSearchPixels(static_cast<size_t>(64 * 16));
            ZHLN::PBR::FillSmaaSearchTex(smaaSearchPixels);
            smaaAreaTexIdx   = CreateTextureInternal(smaaAreaPixels.data(), 160, 560, false);
            smaaSearchTexIdx = CreateTextureInternal(smaaSearchPixels.data(), 64, 16, false);
        });
}

std::expected<void, Error> RenderContext::Impl::SetupUI(GLFWwindow* window) {
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
                .AddDescriptorSetLayout(bindlessLayout.Get())
                .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(UIObjectConstants))
                .Build()
                .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
                .transform([&](auto&& layout) { uiPipelineLayout = std::forward<decltype(layout)>(layout); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            VkFormat swapchainFormat = presentation.swapchain.Get().format;

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

                VkFormat swapchainFormat = presentation.swapchain.Get().format;

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
                                 .depthAttachmentFormat   = VK_FORMAT_D32_SFLOAT,
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
        VkImageViewCreateInfo viewInfo = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext            = {},
            .flags            = {},
            .image            = graphResources.shadowAtlas.image.Handle(),
            .viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format           = VK_FORMAT_D32_SFLOAT,
            .components       = {},
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = i * 6, .layerCount = 6},
        };

        VkImageView rawView = VK_NULL_HANDLE;
        vkCreateImageView(ctx.Device(), &viewInfo, nullptr, &rawView);
        punctualShadowViews[i] = Vk::ImageView(ctx.Device(), rawView);
    }
}

void RenderContext::Impl::InitSkeletalAnimationResources() {
    // Allocate our global Joint storage buffer (Supports 8192 dynamic matrices)
    JPH::Array<JPH::Mat44> identities(8192, JPH::Mat44::sIdentity());
    for (int i = 0; i < 2; ++i) {
        jointBuffers[i] = Vk::Buffer::Create(
            allocator.Get(), sizeof(JPH::Mat44) * 8192, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );

        // Upload identity matrices initially
        auto mapped = jointBuffers[i].Map();
        std::memcpy(mapped.data, identities.data(), identities.size() * sizeof(JPH::Mat44));
    }

    morphDeltasBuffer = Vk::Buffer::Create(
        allocator.Get(), sizeof(float) * 4 * 1000000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU
    );
}

void RenderContext::Impl::InitLightingLUTs() {
    stagingContext = std::make_unique<Vk::StagingContext>(allocator, ctx);
    stagingContext->Begin();

    iblPayload = Vk::IBLProcessor::Bake(*this, *stagingContext);

    ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

    VkImageCreateInfo ltcInfo = {
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

    using namespace Resource;
    const size_t matRawSize = ltc_mat.size() - 128;
    const size_t ampRawSize = ltc_amp.size() - 128;

    auto ltcStaging = Vk::Buffer::Create(allocator.Get(), matRawSize + ampRawSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    auto        mapped       = ltcStaging.Map();
    char* const stageBasePtr = static_cast<char*>(mapped.data);

    struct LUTUploadItem {
        Vk::Image*               targetImage;
        std::span<const uint8_t> rawData;
        size_t                   rawSize;
        size_t                   bufferOffset;
    };

    std::array<LUTUploadItem, 2> uploads = {
        {{.targetImage = &ltcMatImage, .rawData = ltc_mat, .rawSize = matRawSize, .bufferOffset = 0},
         {.targetImage = &ltcAmpImage, .rawData = ltc_amp, .rawSize = ampRawSize, .bufferOffset = matRawSize}}
    };

    for (const auto& item: uploads) {
        *item.targetImage = Vk::Image::Create(allocator.Get(), ltcInfo, VMA_MEMORY_USAGE_GPU_ONLY);

        std::memcpy(stageBasePtr + item.bufferOffset, item.rawData.data() + 128, item.rawSize);

        stagingContext->UploadImage2DBuffer(item.targetImage->Handle(), 64, 64, 1, ltcStaging.Handle(), item.bufferOffset);
    }

    stagingContext->AddBuffer(std::move(ltcStaging));
    stagingContext->ExecuteAsync();

    // 4. Create image views safely outside the async execution line
    ltcMatView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcMatImage.Handle());
    ltcAmpView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcAmpImage.Handle());
}

bool RenderContext::Impl::RecreateTargets(VkExtent2D ext) {
    if (!presentation.Rebuild(ext.width, ext.height)) {
        return false;
    }

    graphResources.sceneColor            = CreateDefaultTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>(ext);
    graphResources.velocityBuffer        = CreateDefaultTarget<VK_FORMAT_R16G16_SFLOAT>(ext);
    accumBuffers[0]                      = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
    accumBuffers[1]                      = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
    graphResources.normalRoughnessBuffer = CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext);
    graphResources.postProcessTarget     = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
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

    graphResources.voxelMedia =
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    graphResources.voxelLight =
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    graphResources.voxelIntegrated =
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    graphResources.bloomThresholdTarget = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2);
    graphResources.bloomDown1           = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4);
    graphResources.bloomDown2           = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8);
    graphResources.bloomDown3           = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext16);
    graphResources.bloomUp2             = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8);
    graphResources.bloomUp1             = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4);
    graphResources.bloomFinalTarget     = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2);

    RecreatePunctualShadowViews();

    // Transition all newly allocated render targets to their correct default layouts
    Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
        std::array colorTargets = {
            graphResources.sceneColor.image.Handle(),
            graphResources.velocityBuffer.image.Handle(),
            accumBuffers[0].image.Handle(),
            accumBuffers[1].image.Handle(),
            graphResources.normalRoughnessBuffer.image.Handle(),
            graphResources.postProcessTarget.image.Handle(),
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
            graphResources.bloomFinalTarget.image.Handle()
        };

        for (auto* const img: colorTargets) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
            cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
        );
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
        );
        std::array targets3D = {
            graphResources.voxelMedia.image.Handle(), graphResources.voxelLight.image.Handle(), graphResources.voxelIntegrated.image.Handle()
        };

        for (auto* const img: targets3D) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    });

    return true;
}

std::expected<void, Error> RenderContext::Impl::BuildHangGpuPipeline() {
    ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = nullptr, .set_layout_count = 0, .push_constants = nullptr, .push_constant_count = 0};

    hangGpuPass.pipelineLayout = Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &pLayoutDesc));

    return LoadAndCreateComputeShader(
               {.path = SHADER_HANG_GPU_HLSL_CS_PATH, .fallback = Resource::hang_gpu_comp, .entryPoint = "CSMain"}, hangGpuPass.pipelineLayout.Get()
    )
        .transform_error([](auto err) -> Error { return err; })
        .transform([&](auto&& pipeline) { hangGpuPass.pipeline = std::forward<decltype(pipeline)>(pipeline); });
}

} // namespace ZHLN
