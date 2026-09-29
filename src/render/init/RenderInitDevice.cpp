// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../OpenGLHacks/HostBlit.hpp"
#include "../PresentationSurface.hpp"
#include "../RenderInternal.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace {

struct HardwareCaps {
    bool supportsDrawIndirectCount = false;
    bool supportsInt64             = false;
    bool supportsMeshShader = false;
    bool supportsMultiviewMeshShader = false;
    bool supportsMeshShaderQueries = false;
    bool supportsPipelineStatisticsQuery = false;
    uint32_t               subgroupSize = 0;
    VkSubgroupFeatureFlags subgroupOps  = 0;
    bool supportsFifoLatestReadyKHR      = false;
    bool supportsFifoLatestReadyEXT      = false;
    bool supportsPresentTiming           = false;
    bool supportsCalibratedTimestampsKHR = false;
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
        const bool hasExt = ZHLN::Vk::QueryDeviceExtensions(_physicalDevice, "VK_KHR_draw_indirect_count").All();
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

    auto ProbePipelineStatisticsQuery(bool& target) && noexcept -> HardwareCapsProber&& {
        VkPhysicalDeviceFeatures2 features2 {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
        target = (features2.features.pipelineStatisticsQuery == VK_TRUE);
        return std::move(*this);
    }

    auto ProbeSubgroups(uint32_t& size, VkSubgroupFeatureFlags& ops) && noexcept -> HardwareCapsProber&& {
        VkPhysicalDeviceSubgroupProperties subgroup {};
        subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2 {};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &subgroup;
        vkGetPhysicalDeviceProperties2(_physicalDevice, &properties2);
        size = subgroup.subgroupSize;
        ops  = subgroup.supportedOperations;
        return std::move(*this);
    }

    auto ProbePresentPacing(bool canPresent, bool& fifoKhr, bool& fifoExt, bool& timing, bool& calibKhr) && noexcept
        -> HardwareCapsProber&& {
        if (!canPresent) {
            return std::move(*this);
        }
        using ZHLN::Vk::QueryDeviceExtensions;
        fifoKhr = QueryDeviceExtensions(_physicalDevice, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME).All();
        fifoExt = QueryDeviceExtensions(_physicalDevice, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME).All();
        if (fifoKhr || fifoExt) {
            const auto fifoFeatures = ZHLN::Vk::QueryFeatureSupport<VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>(_physicalDevice);
            if (fifoFeatures.presentModeFifoLatestReady != VK_TRUE) {
                fifoKhr = false;
                fifoExt = false;
                ZHLN::Log("[RenderInit] FIFO latest-ready extension present but presentModeFifoLatestReady is not advertised; paced policies fall back.");
            }
        } else {
            ZHLN::Log("[RenderInit] FIFO latest-ready present mode not advertised; paced policies fall back to legacy V-blank.");
        }

        timing         = false;
        const bool groupExts = QueryDeviceExtensions(_physicalDevice, VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME).All();
        calibKhr             = QueryDeviceExtensions(_physicalDevice, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME).All();
        const bool calibExt  = QueryDeviceExtensions(_physicalDevice, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME).All();
        if (groupExts && (calibKhr || calibExt)) {
            const auto timingFeatures = ZHLN::Vk::QueryFeatureSupport<VkPhysicalDevicePresentTimingFeaturesEXT>(_physicalDevice);
            const auto id2Features     = ZHLN::Vk::QueryFeatureSupport<VkPhysicalDevicePresentId2FeaturesKHR>(_physicalDevice);
            timing = timingFeatures.presentTiming == VK_TRUE && timingFeatures.presentAtAbsoluteTime == VK_TRUE &&
                     id2Features.presentId2 == VK_TRUE;
            if (!timing) {
                ZHLN::Log(
                    "[RenderInit] Present-timing extensions present but timing/absolute/id2 features are not fully advertised; closed loop disabled."
                );
            }
        } else if (fifoKhr || fifoExt) {
            ZHLN::Log("[RenderInit] VK_EXT_present_timing extension group not fully advertised; latest-ready presents run untimed.");
        }
        return std::move(*this);
    }

  private:
    VkPhysicalDevice _physicalDevice;
    uint32_t         _apiVersion;
};

auto CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;
auto CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;
auto CheckMeshShaderQueriesSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;

auto ProbeHardware(VkPhysicalDevice physicalDevice, uint32_t apiVersion, bool canPresent) noexcept -> HardwareCaps {
    HardwareCaps caps {};
    HardwareCapsProber(physicalDevice, apiVersion)
        .ProbeInt64(caps.supportsInt64)
        .ProbeDrawIndirectCount(caps.supportsDrawIndirectCount)
        .ProbePipelineStatisticsQuery(caps.supportsPipelineStatisticsQuery)
        .ProbeSubgroups(caps.subgroupSize, caps.subgroupOps)
        .ProbePresentPacing(
            canPresent, caps.supportsFifoLatestReadyKHR, caps.supportsFifoLatestReadyEXT, caps.supportsPresentTiming,
            caps.supportsCalibratedTimestampsKHR
        );
    caps.supportsMeshShader          = CheckMeshShaderSupport(physicalDevice);
    caps.supportsMultiviewMeshShader = caps.supportsMeshShader && CheckMultiviewMeshShaderSupport(physicalDevice);
    caps.supportsMeshShaderQueries   = caps.supportsMeshShader && CheckMeshShaderQueriesSupport(physicalDevice);

    constexpr VkSubgroupFeatureFlags kUsedSubgroupOps =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    if ((caps.subgroupOps & kUsedSubgroupOps) != kUsedSubgroupOps) {
        ZHLN::Log(
            "[RenderInit] WARNING: device reports subgroup width {} but supportedOperations={:#x} lacks BASIC/ARITHMETIC/SHUFFLE; "
            "cluster_culling.slang's subgroup scan needs those op classes.",
            caps.subgroupSize, caps.subgroupOps
        );
    } else {
        ZHLN::Log("[RenderInit] Subgroup width {} (supportedOperations={:#x}); cluster scan runs its subgroup path.", caps.subgroupSize, caps.subgroupOps);
    }

    return caps;
}

auto CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    const auto meshExt = ZHLN::Vk::QueryDeviceExtensions(physicalDevice, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    if (!meshExt.All()) {
        ZHLN::Log(
            "[RenderInit] VK_EXT_mesh_shader not present among the {} device extensions reported; using the vertex pipeline.", meshExt.reportedCount
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

    return true;
}

auto CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return meshFeatures.multiviewMeshShader == VK_TRUE;
}

auto CheckMeshShaderQueriesSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return meshFeatures.meshShaderQueries == VK_TRUE;
}

}

namespace ZHLN {

auto CheckRayTracingSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    return ZHLN::Vk::QueryDeviceExtensions(
               physicalDevice, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
               VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    )
        .All();
}

namespace {

auto GetPlatformInstanceExtensions(const PresentationTarget& target) noexcept -> std::expected<Vk::ExtensionResult, ErrorCode> {
    auto builder = Vk::ExtensionBuilder::ForInstance();

    if constexpr (isMac) {
    } else {
        AppendPlatformSurfaceExtensions(builder, target.GetNativeSurface());
    }

    return std::move(builder)
        .Debug(true)
        .OptionalIf("VK_KHR_portability_enumeration", isMac)
        .OptionalIf(
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, !target.IsHeadless() && !target.GetNativeSurface().Valid()
        )
        .Build()
        .transform_error([](auto err) -> ErrorCode { return err; });
}

auto BuildFeatureChain(VkPhysicalDevice physicalDevice, const HardwareCaps& caps, ValidationMode validationMode) noexcept {
    return Vk::FeatureChainBuilder(physicalDevice)
        .Optional<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>([](auto& f) -> auto { f.swapchainMaintenance1 = VK_TRUE; })
        .Optional<VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>([&caps](auto& f) -> auto {
            f.presentModeFifoLatestReady = (caps.supportsFifoLatestReadyKHR || caps.supportsFifoLatestReadyEXT) ? VK_TRUE : VK_FALSE;
        })
        .Optional<VkPhysicalDevicePresentTimingFeaturesEXT>([&caps](auto& f) -> auto {
            f.presentTiming         = caps.supportsPresentTiming ? VK_TRUE : VK_FALSE;
            f.presentAtAbsoluteTime = caps.supportsPresentTiming ? VK_TRUE : VK_FALSE;
            f.presentAtRelativeTime = VK_FALSE;
        })
        .Optional<VkPhysicalDevicePresentId2FeaturesKHR>([&caps](auto& f) -> auto { f.presentId2 = caps.supportsPresentTiming ? VK_TRUE : VK_FALSE; })
        .Require<VkPhysicalDeviceVulkan11Features>([](auto& f) -> auto {
            f.multiview                          = VK_TRUE;
            f.storageBuffer16BitAccess           = VK_TRUE;
            f.uniformAndStorageBuffer16BitAccess = VK_TRUE;
            f.shaderDrawParameters               = VK_TRUE;
        })
        .Require<VkPhysicalDeviceVulkan13Features>([](auto& f) -> auto {
            f.synchronization2               = VK_TRUE;
            f.dynamicRendering               = VK_TRUE;
            f.shaderDemoteToHelperInvocation = VK_TRUE;
        })
        .Require<VkPhysicalDeviceMaintenance5FeaturesKHR>([](auto& f) -> auto { f.maintenance5 = VK_TRUE; })
        .Require<VkPhysicalDeviceVulkan12Features>([&](auto& f) -> auto {
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
        .Optional<VkPhysicalDeviceAccelerationStructureFeaturesKHR>([](auto& f) -> auto { f.accelerationStructure = VK_TRUE; })
        .Optional<VkPhysicalDeviceRayQueryFeaturesKHR>([](auto& f) -> auto { f.rayQuery = VK_TRUE; })
        .Require<VkPhysicalDeviceDescriptorHeapFeaturesEXT>([](auto& f) -> auto { f.descriptorHeap = VK_TRUE; })
        .Optional<VkPhysicalDeviceMeshShaderFeaturesEXT>([&caps](auto& f) -> auto {
            f.taskShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            f.meshShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            f.multiviewMeshShader = caps.supportsMultiviewMeshShader ? VK_TRUE : VK_FALSE;
            f.meshShaderQueries = caps.supportsMeshShaderQueries ? VK_TRUE : VK_FALSE;
        })
        .Require<VkPhysicalDeviceFeatures2>([&](auto& f) -> auto {
            f.features.multiDrawIndirect         = VK_TRUE;
            f.features.samplerAnisotropy         = VK_TRUE;
            f.features.drawIndirectFirstInstance = VK_TRUE;
            f.features.shaderInt64               = caps.supportsInt64 ? VK_TRUE : VK_FALSE;
            f.features.imageCubeArray            = VK_TRUE;
            f.features.shaderInt16               = VK_TRUE;
            f.features.pipelineStatisticsQuery = caps.supportsPipelineStatisticsQuery ? VK_TRUE : VK_FALSE;

            if (validationMode == ZHLN::ValidationMode::GPU) {
                f.features.robustBufferAccess             = VK_TRUE;
                f.features.fragmentStoresAndAtomics       = VK_TRUE;
                f.features.vertexPipelineStoresAndAtomics = VK_TRUE;
                f.features.shaderInt16                    = VK_TRUE;
            }
        })
        .Build();
}

auto GetDeviceExtensions(VkPhysicalDevice physicalDevice, bool noSwapchain, const HardwareCaps& caps) noexcept
    -> std::expected<Vk::ExtensionResult, ErrorCode> {
    auto builder = Vk::ExtensionBuilder::ForDevice(physicalDevice);

    if (!noSwapchain) {
        builder.Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            .Optional(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)
            .Optional(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
        if (caps.supportsFifoLatestReadyKHR) {
            builder.Optional(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
        } else if (caps.supportsFifoLatestReadyEXT) {
            builder.Optional(VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
        }
        const char* calibName = caps.supportsCalibratedTimestampsKHR ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                                                                     : VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        builder.OptionalGroup(
            {VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME, calibName}, caps.supportsPresentTiming
        );
    }

    return builder.OptionalIf("VK_KHR_portability_subset", isMac)
        .OptionalGroup(
            {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
            CheckRayTracingSupport(physicalDevice)
        )
        .Require(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME)
        .Require(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)
        .OptionalGroup({VK_EXT_MESH_SHADER_EXTENSION_NAME}, caps.supportsMeshShader)
        .Build()
        .transform_error([](auto err) -> ErrorCode { return err; });
}

auto SelectPresentationMode(const PresentationTarget& target) noexcept -> PresentationMode {
    if (target.IsHeadless()) {
        return PresentationMode::OffscreenOnly;
    }
    if constexpr (isMac) {
        return PresentationMode::HostBlit;
    } else {
        return PresentationMode::NativeSwapchain;
    }
}

}

RenderContext::RenderContext(PrivateToken , std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Create(
    PresentationTarget& target, const RenderConfig& cfg, FS::FileSystemWatcher* fileSystemWatcher
) noexcept -> std::expected<std::unique_ptr<RenderContext>, ErrorCode> {
    auto impl     = std::make_unique<Impl>(target, fileSystemWatcher);
    impl->appName = cfg.appName;
    impl->pipelineCachePath = cfg.pipelineCachePath;
    impl->enableMeshShading = cfg.enableMeshShading && (std::getenv("ZHLN_NO_MESH_SHADING") == nullptr);

    const PresentationMode mode = SelectPresentationMode(target);
    impl->presentationMode      = mode;

    Vk::Instance            instanceObject;
    VkInstance              instance    = VK_NULL_HANDLE;
    VkSurfaceKHR            raw_surface = VK_NULL_HANDLE;
    int                     width       = 0;
    int                     height      = 0;
    ZHLN_PhysicalDeviceInfo physicalInfo {};

    return GetPlatformInstanceExtensions(target)
        .and_then([&](auto&& inst_exts) -> std::expected<void, ErrorCode> {
            return Vk::Context::Builder()
                .AppName(impl->appName)
                .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                .InstanceExtensions(inst_exts)
                .BuildInstance()
                .transform([&](Vk::Instance inst) -> void {
                    instanceObject = std::move(inst);
                    instance       = instanceObject.Handle();
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (mode == PresentationMode::OffscreenOnly || mode == PresentationMode::HostBlit) {
                width  = 1280;
                height = 720;
                if (const Extent2D fb = target.GetFramebufferExtent(); fb.width > 0 && fb.height > 0) {
                    width  = static_cast<int>(fb.width);
                    height = static_cast<int>(fb.height);
                }
                raw_surface = VK_NULL_HANDLE;
                return {};
            }
            if (!target.IsTTY()) {
                auto surfaceRes = CreateSurfaceFromNative(instance, target.GetNativeSurface());
                if (!surfaceRes) {
                    return std::unexpected(surfaceRes.error());
                }
                raw_surface = surfaceRes->Release();
                if (const Extent2D fb = target.GetFramebufferExtent(); fb.width > 0 && fb.height > 0) {
                    width  = static_cast<int>(fb.width);
                    height = static_cast<int>(fb.height);
                }
                return {};
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return Vk::Context::Builder()
                .Instance(instance)
                .Surface(raw_surface)
                .SelectPhysicalDevice()
                .transform([&](const ZHLN_PhysicalDeviceInfo& info) -> void { physicalInfo = info; });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (target.IsTTY() && mode == PresentationMode::NativeSwapchain) {
                uint32_t modeWidth  = 0;
                uint32_t modeHeight = 0;
                auto     surfaceRes = Vk::CreateDisplaySurface(instance, physicalInfo.handle, modeWidth, modeHeight);
                if (!surfaceRes) {
                    return std::unexpected(surfaceRes.error());
                }
                width       = static_cast<int>(modeWidth);
                height      = static_cast<int>(modeHeight);
                raw_surface = surfaceRes->Release();
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            impl->presenter.surface = Vk::Surface(instance, raw_surface);
            HardwareCaps caps     = ProbeHardware(
                physicalInfo.handle, physicalInfo.properties.properties.apiVersion, mode == PresentationMode::NativeSwapchain
            );
            auto features = BuildFeatureChain(physicalInfo.handle, caps, cfg.validationMode);

            return GetDeviceExtensions(physicalInfo.handle, mode != PresentationMode::NativeSwapchain, caps)
                .and_then([&](auto&& dev_exts) -> std::expected<void, ErrorCode> {
                    const std::vector<const char*>& devExtList = dev_exts;

                    return Vk::Context::Builder()
                        .Instance(std::move(instanceObject))
                        .Surface(raw_surface)
                        .PhysicalDevice(physicalInfo)
                        .DeviceExtensions(devExtList)
                        .DeviceFeatures(features)
                        .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                        .Build()
                        .transform([&](auto&& context) -> auto {
                            impl->ctx         = std::forward<decltype(context)>(context);
                            const auto vendor = static_cast<Vk::GPUVendor>(physicalInfo.properties.properties.vendorID);
                            impl->gpuDiagnostics.Create(
                                vendor, impl->ctx.Device(), impl->ctx.Physical(), Vk::DiagnosticConfig{.crashDumpPath = cfg.crashDumpPath}
                            );
                        });
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if constexpr (isMac) {
                if (mode == PresentationMode::HostBlit) {
                    const bool ok = HostBlit::Init(
                        impl->ctx.Physical(), impl->ctx.Device(), impl->ctx.GraphicsQueue(), physicalInfo.graphics_family
                    );
                    if (!ok) {
                        ZHLN::Log("WARNING: HostBlit presenter failed to initialize; continuing offscreen-only.");
                        impl->presentationMode = PresentationMode::OffscreenOnly;
                    }
                }
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return impl->InitSubsystems(cfg, width, height); })
        .transform([&]() -> std::unique_ptr<ZHLN::RenderContext> {
            impl->BeginShaderObservation();
            return std::make_unique<RenderContext>(PrivateToken {}, std::move(impl));
        });
}

RenderContext::~RenderContext() {
    // Opt-in breadcrumbs around potentially blocking GPU cleanup. A pipeline
    // cache "Saved" line alone does not mean renderer destruction finished.
    const bool traceEnabled = std::getenv("ZHLN_TRACE_TEARDOWN") != nullptr;
    const auto start = std::chrono::steady_clock::now();
    const auto trace = [&](const char* phase) {
        if (traceEnabled) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            ZHLN::Log("[Render teardown] RenderContext {} (+{} ms)", phase, ms);
        }
    };
    if (_impl && (_impl->ctx.Device() != nullptr)) {
        trace("wait idle before destinations");
        if (auto idle = Vk::WaitIdle(_impl->ctx.Device()); !idle) {
            ZHLN::Log("ERROR: Failed to wait for idle while destroying destinations ({})", idle.error());
        }
        trace("destroy destinations");
        _impl->DestroyDestinations();
        if constexpr (isMac) {
            if (_impl->presentationMode == PresentationMode::HostBlit) {
                HostBlit::Shutdown();
            }
        }
        _impl->gpuDiagnostics.Shutdown();
        trace("wait idle before pipeline cache save");
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (!res) {
            ZHLN::Log("ERROR: Failed to wait for idle on device destruction.");
        }
        trace("save pipeline cache");
        Vk::SavePipelineCache(_impl->ctx.Device(), _impl->pipelineCache.Get(), _impl->pipelineCachePath);
        trace("reset submitted staging (fence wait)");
        _impl->submittedStaging.reset();
        trace("RenderContext body complete; Impl teardown follows");
    }
}

}
