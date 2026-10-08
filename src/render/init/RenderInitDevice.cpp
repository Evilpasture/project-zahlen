// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../OpenGLHacks/HostBlit.hpp"
#include "../PresentationSurface.hpp"
#include "../RenderInternal.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstdlib>
#include <vector>

namespace ZHLN {

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
        .OptionalIf(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, !target.IsHeadless())
        .Build()
        .transform_error([](auto err) -> ErrorCode { return err; });
}

// All feature/extension/limit negotiation is owned by the Vulkan layer.
// Required core bits stay required; drawIndirectCount, shaderInt64, and
// pipelineStatisticsQuery are masked independently within their core structs.
auto ConfigureDevice(VkPhysicalDevice physical, bool present, ValidationMode validationMode) {
    constexpr VkSubgroupFeatureFlags kUsedSubgroupOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;

    return Vk::DeviceConfigurator<>(physical)
        .OptionalPresentation(present)
        .OptionalExtension("VK_KHR_portability_subset", isMac)
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
        .RequireExtension<VkPhysicalDeviceMaintenance5FeaturesKHR>(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, [](auto& f) { f.maintenance5 = VK_TRUE; })
        .RequireWithOptional<VkPhysicalDeviceVulkan12Features>(
            [validationMode](auto& f) {
                f.descriptorIndexing                           = VK_TRUE;
                f.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
                f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                f.descriptorBindingPartiallyBound              = VK_TRUE;
                f.runtimeDescriptorArray                       = VK_TRUE;
                f.bufferDeviceAddress                          = VK_TRUE;
                f.hostQueryReset                               = VK_TRUE;
                f.timelineSemaphore                            = VK_TRUE;
                f.uniformAndStorageBuffer8BitAccess            = VK_TRUE;
                f.shaderFloat16                                = VK_TRUE;
                if (validationMode == ValidationMode::GPU) {
                    f.scalarBlockLayout            = VK_TRUE;
                    f.storageBuffer8BitAccess      = VK_TRUE;
                    f.shaderInt8                   = VK_TRUE;
                    f.vulkanMemoryModel            = VK_TRUE;
                    f.vulkanMemoryModelDeviceScope = VK_TRUE;
                }
            },
            [](auto& f) { f.drawIndirectCount = VK_TRUE; }
        )
        .OptionalRayTracing()
        .RequireExtension<VkPhysicalDeviceDescriptorHeapFeaturesEXT>(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, [](auto& f) { f.descriptorHeap = VK_TRUE; })
        .OptionalMeshShaders()
        .RequireWithOptional<VkPhysicalDeviceFeatures2>(
            [validationMode](auto& f) {
                f.features.multiDrawIndirect         = VK_TRUE;
                f.features.samplerAnisotropy         = VK_TRUE;
                f.features.drawIndirectFirstInstance = VK_TRUE;
                f.features.imageCubeArray            = VK_TRUE;
                f.features.shaderInt16               = VK_TRUE;
                if (validationMode == ValidationMode::GPU) {
                    f.features.robustBufferAccess             = VK_TRUE;
                    f.features.fragmentStoresAndAtomics       = VK_TRUE;
                    f.features.vertexPipelineStoresAndAtomics = VK_TRUE;
                }
            },
            [](auto& f) {
                f.features.shaderInt64             = VK_TRUE;
                f.features.pipelineStatisticsQuery = VK_TRUE;
            }
        )
        .SubgroupDiagnostics(kUsedSubgroupOps)
        .Build();
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

} // namespace

RenderContext::RenderContext(PrivateToken /*unused*/, std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Create(PresentationTarget& target, const RenderConfig& cfg, ZHLN::Optional<FS::FileSystemWatcher&> fileSystemWatcher) noexcept
    -> std::expected<std::unique_ptr<RenderContext>, ErrorCode> {
    auto impl               = std::make_unique<Impl>(target, fileSystemWatcher);
    impl->appName           = cfg.appName;
    impl->pipelineCachePath = cfg.pipelineCachePath;
    impl->enableMeshShading = cfg.enableMeshShading && (std::getenv("ZHLN_NO_MESH_SHADING") == nullptr);

    const PresentationMode mode = SelectPresentationMode(target);
    impl->presentationMode      = mode;

    Vk::Instance           instanceObject;
    VkInstance             instance    = VK_NULL_HANDLE;
    VkSurfaceKHR           raw_surface = VK_NULL_HANDLE;
    int                    width       = 0;
    int                    height      = 0;
    Vk::PhysicalDeviceInfo physicalInfo {};

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
                .transform([&](const Vk::PhysicalDeviceInfo& info) -> void { physicalInfo = info; });
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
            return ConfigureDevice(physicalInfo.handle, mode == PresentationMode::NativeSwapchain, cfg.validationMode)
                .and_then([&](auto&& setup) -> std::expected<void, ErrorCode> {
                    const std::vector<const char*>& devExtList = setup.extensions;
                    return Vk::Context::Builder()
                        .Instance(std::move(instanceObject))
                        .Surface(raw_surface)
                        .PhysicalDevice(physicalInfo)
                        .DeviceExtensions(devExtList)
                        .DeviceFeatures(setup.features)
                        .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                        .Build()
                        .transform([&](auto&& context) -> auto {
                            impl->ctx         = std::forward<decltype(context)>(context);
                            const auto vendor = static_cast<Vk::GPUVendor>(physicalInfo.properties.properties.vendorID);
                            impl->gpuDiagnostics.Create(
                                vendor, impl->ctx.Device(), impl->ctx.Physical(), Vk::DiagnosticConfig {.crashDumpPath = cfg.crashDumpPath}
                            );
                        });
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if constexpr (isMac) {
                if (mode == PresentationMode::HostBlit) {
                    const bool ok = HostBlit::Init(impl->ctx.Physical(), impl->ctx.Device(), impl->ctx.GraphicsQueue(), physicalInfo.graphicsFamily);
                    if (!ok) {
                        ZHLN::LogWarning("HostBlit presenter failed to initialize; continuing offscreen-only.");
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

RenderContext::~RenderContext()
// TODO(Evilpasture): Add an explicit RenderContext::Destroy method.
{
    if (_impl && (_impl->ctx.Device() != nullptr)) {
        if (auto idle = Vk::WaitIdle(_impl->ctx.Device()); !idle) {
            ZHLN::LogError("Failed to wait for idle while destroying destinations ({})", idle.error());
        }
        _impl->DestroyDestinations();
        if constexpr (isMac) {
            if (_impl->presentationMode == PresentationMode::HostBlit) {
                HostBlit::Shutdown();
            }
        }
        _impl->gpuDiagnostics.Shutdown();
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (!res) {
            ZHLN::LogError("Failed to wait for idle on device destruction.");
        }
        Vk::SavePipelineCache(_impl->ctx.Device(), _impl->pipelineCache.Get(), _impl->pipelineCachePath);
        _impl->submittedStaging.reset();
    }
}

} // namespace ZHLN
