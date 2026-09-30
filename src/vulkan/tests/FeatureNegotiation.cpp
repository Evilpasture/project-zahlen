// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Internal Vulkan-only negotiation tests: mocked physical-device queries and
// an injected extension catalog. No ICD, window, or GPU is needed.
#include "Rendering.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct MockSupport {
    bool taskShader = true;
    bool multiviewMeshShader = false;
    bool rayQuery = true;
    bool presentAtAbsoluteTime = true;
    bool meshLimitsSufficient = true;
    bool multiDrawIndirect = true;
    bool drawIndirectCount = false;
    bool dynamicRenderingUnused = true;
    bool nullDescriptor = true;
    bool robustBufferAccess2 = false;
    int  queries = 0;
    int  failures = 0;
};

MockSupport g_support;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FeatureNegotiation: %s\n", message);
        ++g_support.failures;
    }
}

void VKAPI_CALL MockFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures2* root) {
    ++g_support.queries;
    Check(root->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "feature query must start at Features2");
    root->features.multiDrawIndirect = g_support.multiDrawIndirect ? VK_TRUE : VK_FALSE;
    root->features.shaderInt64 = VK_FALSE;
    root->features.pipelineStatisticsQuery = VK_TRUE;

    if (root->pNext == nullptr) {
        return;
    }
    VkStructureType type = VK_STRUCTURE_TYPE_MAX_ENUM;
    void* next = nullptr;
    std::memcpy(&type, root->pNext, sizeof(type));
    std::memcpy(&next, static_cast<const char*>(root->pNext) + offsetof(VkPhysicalDeviceFeatures2, pNext), sizeof(next));
    Check(next == nullptr && type != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "extension query must have exactly one non-root struct");
    switch (type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            auto* f = static_cast<VkPhysicalDeviceVulkan12Features*>(root->pNext);
            f->bufferDeviceAddress = VK_TRUE;
            f->drawIndirectCount = g_support.drawIndirectCount ? VK_TRUE : VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT: {
            auto* f = static_cast<VkPhysicalDeviceMeshShaderFeaturesEXT*>(root->pNext);
            f->taskShader = g_support.taskShader ? VK_TRUE : VK_FALSE;
            f->meshShader = VK_TRUE;
            f->multiviewMeshShader = g_support.multiviewMeshShader ? VK_TRUE : VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR:
            static_cast<VkPhysicalDeviceAccelerationStructureFeaturesKHR*>(root->pNext)->accelerationStructure = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR:
            static_cast<VkPhysicalDeviceRayQueryFeaturesKHR*>(root->pNext)->rayQuery = g_support.rayQuery ? VK_TRUE : VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT:
            static_cast<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT*>(root->pNext)->dynamicRenderingUnusedAttachments =
                g_support.dynamicRenderingUnused ? VK_TRUE : VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
            auto* f = static_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(root->pNext);
            f->nullDescriptor = g_support.nullDescriptor ? VK_TRUE : VK_FALSE;
            f->robustBufferAccess2 = g_support.robustBufferAccess2 ? VK_TRUE : VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR:
            static_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR*>(root->pNext)->swapchainMaintenance1 = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR:
            static_cast<VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR*>(root->pNext)->presentModeFifoLatestReady = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT: {
            auto* f = static_cast<VkPhysicalDevicePresentTimingFeaturesEXT*>(root->pNext);
            f->presentTiming = VK_TRUE;
            f->presentAtAbsoluteTime = g_support.presentAtAbsoluteTime ? VK_TRUE : VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR:
            static_cast<VkPhysicalDevicePresentId2FeaturesKHR*>(root->pNext)->presentId2 = VK_TRUE;
            break;
        default:
            Check(false, "unexpected feature query");
            break;
    }
}

void VKAPI_CALL MockProperties(VkPhysicalDevice, VkPhysicalDeviceProperties2* properties) {
    Check(properties->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, "property query must start at Properties2");
    if (properties->pNext == nullptr) {
        Check(false, "missing mesh properties");
        return;
    }
    auto* mesh = static_cast<VkPhysicalDeviceMeshShaderPropertiesEXT*>(properties->pNext);
    Check(mesh->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT, "wrong mesh properties type");
    mesh->maxMeshOutputVertices = g_support.meshLimitsSufficient ? 64 : 16;
    mesh->maxMeshOutputPrimitives = 124;
    mesh->maxTaskWorkGroupInvocations = 32;
    mesh->maxMeshWorkGroupInvocations = 64;
}

[[nodiscard]] auto Catalog(std::span<const std::string_view> names) -> ZHLN::Vk::ExtensionBuilder {
    return ZHLN::Vk::ExtensionBuilder::ForAvailable(names);
}

[[nodiscard]] bool Enabled(const ZHLN::Vk::ExtensionResult& extensions, std::string_view name) {
    const auto& list = static_cast<const std::vector<const char*>&>(extensions);
    return std::ranges::any_of(list, [name](const char* entry) { return entry && name == entry; });
}

} // namespace

int main() {
    const auto oldFeatures = vkGetPhysicalDeviceFeatures2;
    const auto oldProperties = vkGetPhysicalDeviceProperties2;
    vkGetPhysicalDeviceFeatures2 = &MockFeatures;
    vkGetPhysicalDeviceProperties2 = &MockProperties;
    const auto gpu = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t {1});

    { // A missing optional core bit cannot drop the required core struct.
        auto setup = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog({}))
            .RequireWithOptional<VkPhysicalDeviceFeatures2>(
                [](auto& f) { f.features.multiDrawIndirect = VK_TRUE; },
                [](auto& f) { f.features.shaderInt64 = VK_TRUE; f.features.pipelineStatisticsQuery = VK_TRUE; }
            ).Build();
        Check(setup.has_value(), "required + optional core feature negotiation");
        if (setup) {
            const auto* core = setup->features.Find<VkPhysicalDeviceFeatures2>();
            Check(core && core->features.multiDrawIndirect && core->features.pipelineStatisticsQuery && !core->features.shaderInt64,
                  "mask shaderInt64 without dropping other core bits");
            Check(setup->features.GetRoot()->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "single core root in device chain");
        }
        auto optional = ZHLN::Vk::FeatureChainBuilder(gpu)
            .Optional<VkPhysicalDeviceFeatures2>([](auto& f) { f.features.multiDrawIndirect = VK_TRUE; f.features.shaderInt64 = VK_TRUE; })
            .Build();
        Check(optional.Find<VkPhysicalDeviceFeatures2>() != nullptr, "Optional<Features2> queries the core root directly");
        Check(optional.Find<VkPhysicalDeviceFeatures2>()->features.shaderInt64 == VK_FALSE, "Optional masks missing core bit");
    }
    { // Optional core 1.2 bit is masked within the one required 1.2 struct.
        auto setup = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog({}))
            .RequireWithOptional<VkPhysicalDeviceVulkan12Features>(
                [](auto& f) { f.bufferDeviceAddress = VK_TRUE; }, [](auto& f) { f.drawIndirectCount = VK_TRUE; }
            ).Build();
        Check(setup.has_value(), "required + optional Vulkan 1.2 feature negotiation");
        if (setup) {
            const auto* f = setup->features.Find<VkPhysicalDeviceVulkan12Features>();
            Check(f && f->bufferDeviceAddress && !f->drawIndirectCount, "mask drawIndirectCount, keep bufferDeviceAddress");
        }
    }
    { // Required bits/extensions must fail before vkCreateDevice, not be silently masked.
        g_support.multiDrawIndirect = false;
        auto missingBit = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog({}))
            .Require<VkPhysicalDeviceFeatures2>([](auto& f) { f.features.multiDrawIndirect = VK_TRUE; }).Build();
        Check(!missingBit, "missing required feature is an error");
        g_support.multiDrawIndirect = true;
        const int before = g_support.queries;
        auto missingExtension = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog({}))
            .RequireExtension<VkPhysicalDeviceMeshShaderFeaturesEXT>(
                VK_EXT_MESH_SHADER_EXTENSION_NAME, [](auto& f) { f.meshShader = VK_TRUE; }
            ).Build();
        Check(!missingExtension && g_support.queries == before, "missing required extension skips feature query and fails");
    }
    { // Backend requirements are checked, optional bits and names paired.
        constexpr std::array names {
            std::string_view {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME},
            std::string_view {VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME},
            std::string_view {VK_EXT_ROBUSTNESS_2_EXTENSION_NAME}
        };
        const auto configure = [gpu](ZHLN::Vk::ExtensionBuilder catalog) {
            return ZHLN::Vk::DeviceConfigurator<>(gpu, std::move(catalog))
                .RequireExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)
                .RequireExtension<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>(
                    VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME,
                    [](auto& f) { f.dynamicRenderingUnusedAttachments = VK_TRUE; }
                )
                .OptionalExtension<VkPhysicalDeviceRobustness2FeaturesEXT>(
                    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, [](auto& f) { f.nullDescriptor = VK_TRUE; f.robustBufferAccess2 = VK_TRUE; }
                ).Build();
        };
        auto backend = configure(Catalog(names));
        Check(backend && backend->features.Find<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>() &&
                  backend->features.Find<VkPhysicalDeviceRobustness2FeaturesEXT>() &&
                  Enabled(backend->extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME), "backend features require their extensions");
        if (backend) {
            const auto* robust = backend->features.Find<VkPhysicalDeviceRobustness2FeaturesEXT>();
            Check(robust && robust->nullDescriptor && !robust->robustBufferAccess2,
                  "partial optional backend bits do not disable supported null descriptors");
        }
        g_support.nullDescriptor = false;
        auto noRobust = configure(Catalog(names));
        Check(noRobust && !noRobust->features.Find<VkPhysicalDeviceRobustness2FeaturesEXT>() &&
                  !Enabled(noRobust->extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME), "unsupported optional backend feature omits extension");
        g_support.nullDescriptor = true;
        g_support.dynamicRenderingUnused = false;
        Check(!configure(Catalog(names)), "unsupported required backend feature fails negotiation");
        g_support.dynamicRenderingUnused = true;
    }
    { // Extension activation, paired mesh feature bits, and property limits.
        constexpr std::array names {std::string_view {VK_EXT_MESH_SHADER_EXTENSION_NAME}};
        auto mesh = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalMeshShaders().Build();
        Check(mesh.has_value(), "mesh shader negotiation");
        if (mesh) {
            const auto* f = mesh->features.Find<VkPhysicalDeviceMeshShaderFeaturesEXT>();
            Check(f && f->taskShader && f->meshShader && !f->multiviewMeshShader, "mask multiview without dropping mesh shader");
            Check(Enabled(mesh->extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME), "mesh extension accompanies features");
            const auto snapshot = mesh->features.SnapshotEnabled();
            const auto* enabled = ZHLN::Vk::FindEnabledFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>(snapshot);
            Check(enabled && enabled->meshShader && enabled->taskShader && !enabled->multiviewMeshShader && enabled->pNext == nullptr,
                  "enabled-feature snapshot contains only masked bits, with no dangling chain pointer");
        }
        g_support.taskShader = false;
        auto noTask = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalMeshShaders().Build();
        Check(noTask && !noTask->features.Find<VkPhysicalDeviceMeshShaderFeaturesEXT>() &&
                  !Enabled(noTask->extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME), "taskShader and meshShader are an atomic path");
        if (noTask) {
            Check(noTask->features.GetRoot() == nullptr && noTask->features.SnapshotEnabled().empty(),
                  "inactive optional mesh struct is absent from chain and snapshot");
        }
        g_support.taskShader = true;
        g_support.meshLimitsSufficient = false;
        auto lowLimits = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalMeshShaders().Build();
        Check(lowLimits && !lowLimits->features.Find<VkPhysicalDeviceMeshShaderFeaturesEXT>() &&
                  !Enabled(lowLimits->extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME), "mesh limits gate both feature and extension");
        g_support.meshLimitsSufficient = true;
        const int before = g_support.queries;
        auto missing = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog({})).OptionalMeshShaders().Build();
        Check(missing && g_support.queries == before && !Enabled(missing->extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME),
              "absent extension does not query an unsupported feature struct");
    }
    { // A ray query is useful only if the whole extension+feature group works.
        constexpr std::array names {
            std::string_view {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME},
            std::string_view {VK_KHR_RAY_QUERY_EXTENSION_NAME},
            std::string_view {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME}
        };
        g_support.rayQuery = false;
        auto noRay = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalRayTracing().Build();
        Check(noRay && !noRay->features.Find<VkPhysicalDeviceAccelerationStructureFeaturesKHR>() &&
                  !Enabled(noRay->extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME), "partial ray-query group is disabled");
        g_support.rayQuery = true;
        constexpr std::array withoutHostOps {
            std::string_view {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME}, std::string_view {VK_KHR_RAY_QUERY_EXTENSION_NAME}
        };
        const int before = g_support.queries;
        auto missingExt = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(withoutHostOps)).OptionalRayTracing().Build();
        Check(missingExt && g_support.queries == before && !Enabled(missingExt->extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME),
              "missing ray-query dependency skips both feature queries");
        auto ray = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalRayTracing().Build();
        Check(ray && ray->features.Find<VkPhysicalDeviceAccelerationStructureFeaturesKHR>() &&
                  ray->features.Find<VkPhysicalDeviceRayQueryFeaturesKHR>() &&
                  Enabled(ray->extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME), "complete ray-query group is enabled");
        if (ray) {
            Check(ray->features.SnapshotEnabled().size() == 2, "ray-query snapshot contains both feature structs");
            const void* root = ray->features.GetRoot();
            VkStructureType firstType = VK_STRUCTURE_TYPE_MAX_ENUM;
            const void* second = nullptr;
            std::memcpy(&firstType, root, sizeof(firstType));
            std::memcpy(&second, static_cast<const char*>(root) + offsetof(VkPhysicalDeviceFeatures2, pNext), sizeof(second));
            VkStructureType secondType = VK_STRUCTURE_TYPE_MAX_ENUM;
            if (second != nullptr) {
                std::memcpy(&secondType, second, sizeof(secondType));
            }
            Check(firstType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR &&
                      secondType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
                  "composite features are linked in the Vulkan pNext chain");
        }
    }
    { // Present timing requires two structs and one of two calibrated-timestamp extensions.
        constexpr std::array names {
            std::string_view {VK_KHR_SWAPCHAIN_EXTENSION_NAME}, std::string_view {VK_EXT_PRESENT_TIMING_EXTENSION_NAME},
            std::string_view {VK_KHR_PRESENT_ID_2_EXTENSION_NAME}, std::string_view {VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME},
            std::string_view {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME},
            std::string_view {VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME}
        };
        g_support.presentAtAbsoluteTime = false;
        auto untimed = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalPresentation(true).Build();
        Check(untimed && !untimed->features.Find<VkPhysicalDevicePresentTimingFeaturesEXT>() &&
                  !Enabled(untimed->extensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME), "timing group disabled when absolute timing is absent");
        g_support.presentAtAbsoluteTime = true;
        auto paced = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalPresentation(true).Build();
        Check(paced && paced->features.Find<VkPhysicalDevicePresentTimingFeaturesEXT>() &&
                  paced->features.Find<VkPhysicalDevicePresentId2FeaturesKHR>() &&
                  Enabled(paced->extensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) &&
                  !Enabled(paced->extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME), "complete timing group prefers KHR timestamps");
        Check(paced && paced->features.Find<VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>() &&
                  Enabled(paced->extensions, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME), "FIFO EXT fallback enables the same feature bit");
        constexpr std::array withoutCalibration {
            std::string_view {VK_KHR_SWAPCHAIN_EXTENSION_NAME}, std::string_view {VK_EXT_PRESENT_TIMING_EXTENSION_NAME},
            std::string_view {VK_KHR_PRESENT_ID_2_EXTENSION_NAME}
        };
        auto missingCalib = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(withoutCalibration)).OptionalPresentation(true).Build();
        Check(missingCalib && !missingCalib->features.Find<VkPhysicalDevicePresentTimingFeaturesEXT>() &&
                  !Enabled(missingCalib->extensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME), "missing calibrated timestamps disables timing group");
        auto headless = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(names)).OptionalPresentation(false).Build();
        Check(headless && !headless->features.Find<VkPhysicalDevicePresentTimingFeaturesEXT>() &&
                  !Enabled(headless->extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME), "headless device has no present extensions or features");
        constexpr std::array missingSwapchain {std::string_view {VK_EXT_PRESENT_TIMING_EXTENSION_NAME}};
        auto noSwapchain = ZHLN::Vk::DeviceConfigurator<>(gpu, Catalog(missingSwapchain)).OptionalPresentation(true).Build();
        Check(!noSwapchain, "presenting requires the swapchain extension");
    }

    vkGetPhysicalDeviceFeatures2 = oldFeatures;
    vkGetPhysicalDeviceProperties2 = oldProperties;
    if (g_support.failures == 0) {
        std::puts("Vulkan feature negotiation: OK (mock physical device)");
    }
    return g_support.failures == 0 ? 0 : 1;
}
