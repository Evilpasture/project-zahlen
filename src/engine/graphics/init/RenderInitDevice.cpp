// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitDevice.cpp
#include "../../TTYBackend.hpp"
#include "../RenderInternal.hpp"
#include "backends/imgui_impl_glfw.h"
#include "imgui.h"
#include <Features.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
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

auto CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;
auto CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;

auto ProbeHardware(VkPhysicalDevice physicalDevice, uint32_t apiVersion) noexcept -> HardwareCaps {
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
auto CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
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

auto CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return meshFeatures.multiviewMeshShader == VK_TRUE;
}

} // namespace

namespace ZHLN {

auto CheckRayTracingSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    return ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
           ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
           ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
}

namespace {

auto GetPlatformInstanceExtensions(Window& window) noexcept -> std::expected<Vk::ExtensionResult, Error> {
    auto builder = Vk::ExtensionBuilder::ForInstance();

    if (window.IsHeadless()) {
        // True headless mode: no surface extensions required. GLFW is not
        // initialised, so we must not call any GLFW functions here.
    } else if (window.IsTTY()) {
        for (const auto ext: TTYBackend::GetRequiredInstanceExtensions()) {
            builder.Require(ext);
        }
    } else {
        glfwSetErrorCallback([](int error, const char* description) -> void { ZHLN::Log("[GLFW Error] Code {}: {}", error, description); });

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
        .Optional<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>([](auto& f) -> auto { f.swapchainMaintenance1 = VK_TRUE; })
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
        .Optional<VkPhysicalDeviceRobustness2FeaturesEXT>([validationMode](auto& f) -> auto {
            f.nullDescriptor = VK_TRUE;

            if (validationMode == ZHLN::ValidationMode::GPU) {
                f.robustBufferAccess2 = VK_TRUE;
                f.robustImageAccess2  = VK_TRUE;
            }
        })
        // VK_EXT_descriptor_heap: the whole scene binding model now lives in
        // descriptor heaps; the legacy set path remains only for passes that
        // have not been ported yet (post-processing, volumetric, ImGui, ...).
        .Require<VkPhysicalDeviceDescriptorHeapFeaturesEXT>([](auto& f) -> auto { f.descriptorHeap = VK_TRUE; })
        // Pipelines declare a stencil attachment format derived from the depth
        // format, but only some passes actually bind stencil; this feature lets
        // them draw inside stencil-less render passes (and stencil-less
        // secondary command buffers) without format-mismatch VUIDs.
        .Require<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>([](auto& f) -> auto { f.dynamicRenderingUnusedAttachments = VK_TRUE; })
        // VK_EXT_mesh_shader. multiviewMeshShader lets the shadow pass render
        // all cascades from a single dispatch; it is only requested when the
        // device actually supports mesh shading, because the feature struct
        // must not be chained on a device that lacks the extension.
        .Optional<VkPhysicalDeviceMeshShaderFeaturesEXT>([&caps](auto& f) -> auto {
            f.taskShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            f.meshShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            // Only requested when the device actually has it: one unsupported
            // bit would make FeatureChain::Optional discard the entire struct,
            // leaving the extension enabled but task/mesh shading OFF.
            f.multiviewMeshShader = caps.supportsMultiviewMeshShader ? VK_TRUE : VK_FALSE;
        })
        .Require<VkPhysicalDeviceFeatures2>([&](auto& f) -> auto {
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

auto GetDeviceExtensions(VkPhysicalDevice physicalDevice, bool isHeadless, bool meshShaderSupported) noexcept -> std::expected<Vk::ExtensionResult, Error> {
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

} // namespace

RenderContext::RenderContext(PrivateToken /*unused*/, std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Create(Window& window, const RenderConfig& cfg) noexcept -> std::expected<std::unique_ptr<RenderContext>, Error> {
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
                .transform([&](VkInstance inst) -> void { instance = inst; });
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (!window.IsTTY() && !window.IsHeadless()) {
                return window.CreateVulkanSurface(instance, nullptr, width, height)
                    .transform_error([](auto) -> Error { return RenderInitError::SurfaceCreationFailed; })
                    .transform([&](void* surface) -> void { raw_surface = static_cast<VkSurfaceKHR>(surface); });
            }
            if (window.IsHeadless()) {
                // Headless: obtain offscreen dimensions without creating a VkSurfaceKHR
                return window.CreateVulkanSurface(instance, nullptr, width, height)
                    .transform_error([](auto) -> Error { return RenderInitError::SurfaceCreationFailed; })
                    .transform([&](void* /*surface*/) -> void { raw_surface = VK_NULL_HANDLE; });
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            return Vk::Context::Builder()
                .Instance(instance)
                .Surface(raw_surface)
                .SelectPhysicalDevice()
                .transform([&](const ZHLN_PhysicalDeviceInfo& info) -> void { physicalInfo = info; });
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (window.IsTTY()) {
                return window.CreateVulkanSurface(instance, physicalInfo.handle, width, height)
                    .transform_error([](auto) -> Error { return RenderInitError::SurfaceCreationFailed; })
                    .transform([&](void* surface) -> void { raw_surface = static_cast<VkSurfaceKHR>(surface); });
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            impl->surface         = Vk::Surface(instance, raw_surface);
            HardwareCaps caps     = ProbeHardware(physicalInfo.handle, physicalInfo.properties.properties.apiVersion);
            // Plumb through to the render passes: the multiview cascade shadow
            // pass may only bind task/mesh pipelines that read SV_ViewID when
            // the multiviewMeshShader feature was actually enabled.
            impl->multiviewMeshShaderEnabled = caps.supportsMultiviewMeshShader;
            auto         features            = BuildFeatureChain(physicalInfo.handle, caps, cfg.validationMode);

            return GetDeviceExtensions(physicalInfo.handle, window.IsHeadless(), caps.supportsMeshShader)
                .and_then([&](auto&& dev_exts) -> std::expected<void, Error> {
                    const std::vector<const char*>& devExtList = dev_exts;

                    return Vk::Context::Builder()
                        .Instance(instance)
                        .Surface(raw_surface)
                        .PhysicalDevice(physicalInfo)
                        .DeviceExtensions(devExtList)
                        .DeviceFeatures(features.GetRoot())
                        .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                        .Build()
                        .transform([&](auto&& context) -> auto {
                            impl->ctx         = std::forward<decltype(context)>(context);
                            const auto vendor = static_cast<Vk::GPUVendor>(physicalInfo.properties.properties.vendorID);
                            impl->gpuDiagnostics.Create(vendor, impl->ctx.Device(), impl->ctx.Physical());
                        });
                });
        })
        .and_then([&]() -> std::expected<void, Error> { return impl->InitSubsystems(cfg, width, height); })
        .transform([&]() -> std::unique_ptr<ZHLN::RenderContext> { return std::make_unique<RenderContext>(PrivateToken {}, std::move(impl)); });
}

RenderContext::~RenderContext() {
    if (_impl && (_impl->ctx.Device() != nullptr)) {
        _impl->gpuDiagnostics.Shutdown();
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (!res) {
            ZHLN::Log("ERROR: Failed to wait for idle on device destruction.");
        }
        _impl->stagingContext.reset();

        // --- SAFETY: Only shut down ImGui if it was actually initialized ---
        if (ImGui::GetCurrentContext() != nullptr) {
            if (!_impl->window.IsHeadless() && !_impl->window.IsTTY()) {
                ImGui_ImplGlfw_Shutdown();
            }
            ImGui::DestroyContext();
        }
    }
}

} // namespace ZHLN
