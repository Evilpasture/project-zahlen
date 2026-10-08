// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Instance.hpp"
#include "../diagnostics/GPUAddressTracker.hpp"
#include "Extensions.hpp"
#include <VkError.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

std::atomic<Instance*>       Instance::s_active {nullptr};
std::atomic<DiagnosticsSink> Instance::s_registered_sink {DiagnosticsSink {}};

namespace {

[[nodiscard]] auto EnumerateInstanceLayers() noexcept -> std::vector<VkLayerProperties> {
    std::vector<VkLayerProperties> layers;
    for (;;) {
        uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
            return {};
        }
        layers.resize(count);
        const VkResult result = vkEnumerateInstanceLayerProperties(&count, layers.data());
        if (result == VK_SUCCESS) {
            layers.resize(count);
            return layers;
        }
        if (result != VK_INCOMPLETE) {
            return {};
        }
    }
}

[[nodiscard]] auto HasLayer(const std::span<const VkLayerProperties> layers, const std::string_view name) noexcept -> bool {
    return std::ranges::any_of(layers, [name](const VkLayerProperties& layer) { return name == layer.layerName; });
}

[[nodiscard]] auto ContainsAny(const std::string_view text, const std::span<const std::string_view> needles) noexcept -> bool {
    return std::ranges::any_of(needles, [text](const std::string_view needle) { return text.find(needle) != std::string_view::npos; });
}

} // namespace

Instance::Instance() noexcept {
    // Construction stays allocation-free: the telemetry state (DebugState) is
    // set up explicitly by Create, where an allocation failure can be reported.
}

void Instance::UseDiagnostics(const DiagnosticsSink sink) noexcept {
    s_registered_sink.store(sink.Valid() ? sink : DiagnosticsSink {}, std::memory_order::release);
}

void Instance::RebindDebugState() noexcept {
    if (_debugState != nullptr) {
        _debugState->owner = this;
    }
}

void Instance::Destroy() noexcept {
    if (_handle != VK_NULL_HANDLE) {
        if (_addressBindingMessenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
            vkDestroyDebugUtilsMessengerEXT(_handle, _addressBindingMessenger, nullptr);
        }
        if (_messenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
            vkDestroyDebugUtilsMessengerEXT(_handle, _messenger, nullptr);
        }
        vkDestroyInstance(_handle, nullptr);
        _handle                  = VK_NULL_HANDLE;
        _messenger               = VK_NULL_HANDLE;
        _addressBindingMessenger = VK_NULL_HANDLE;
    }

    Instance* expected = this;
    s_active.compare_exchange_strong(expected, nullptr, std::memory_order::release, std::memory_order::relaxed);
}

Instance::~Instance() noexcept {
    Destroy();
}

Instance::Instance(Instance&& other) noexcept:
    _handle(std::exchange(other._handle, VK_NULL_HANDLE)), _messenger(std::exchange(other._messenger, VK_NULL_HANDLE)),
    _addressBindingMessenger(std::exchange(other._addressBindingMessenger, VK_NULL_HANDLE)), _debugState(std::move(other._debugState)),
    _validationErrors(other._validationErrors.load(std::memory_order::relaxed)), _deviceLost(other._deviceLost.load(std::memory_order::relaxed)),
    _validationTarget(other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget),
    _deviceLostTarget(other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget) {
    RebindDebugState();
    if (s_active.load(std::memory_order::acquire) == &other) {
        s_active.store(this, std::memory_order::release);
    }
    other._validationErrors.store(0, std::memory_order::relaxed);
    other._deviceLost.store(0, std::memory_order::relaxed);
    other._validationTarget = &other._validationErrors;
    other._deviceLostTarget = &other._deviceLost;
}

auto Instance::operator=(Instance&& other) noexcept -> Instance& {
    if (this != &other) {
        Destroy();

        _handle                  = std::exchange(other._handle, VK_NULL_HANDLE);
        _messenger               = std::exchange(other._messenger, VK_NULL_HANDLE);
        _addressBindingMessenger = std::exchange(other._addressBindingMessenger, VK_NULL_HANDLE);
        _debugState              = std::move(other._debugState);
        _validationErrors        = other._validationErrors.load(std::memory_order::relaxed);
        _deviceLost              = other._deviceLost.load(std::memory_order::relaxed);
        _validationTarget        = other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget;
        _deviceLostTarget        = other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget;

        RebindDebugState();
        if (s_active.load(std::memory_order::acquire) == &other) {
            s_active.store(this, std::memory_order::release);
        }
        other._validationErrors.store(0, std::memory_order::relaxed);
        other._deviceLost.store(0, std::memory_order::relaxed);
        other._validationTarget = &other._validationErrors;
        other._deviceLostTarget = &other._deviceLost;
    }
    return *this;
}

auto VKAPI_CALL Instance::DebugCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    const VkDebugUtilsMessageTypeFlagsEXT        type,
    const VkDebugUtilsMessengerCallbackDataEXT*  callbackData,
    void*                                        userData
) noexcept -> VkBool32 {
    if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) != 0) {
        if (callbackData != nullptr) {
            for (const auto* node = static_cast<const VkBaseInStructure*>(callbackData->pNext); node != nullptr; node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
                    GPUAddressTracker::Get().OnBindingEvent(*reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(node), callbackData);
                    break;
                }
            }
        }
        return VK_FALSE;
    }

    auto* const state = static_cast<DebugState*>(userData);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 && state != nullptr && state->owner != nullptr) {
        state->owner->_validationTarget->fetch_add(1, std::memory_order::relaxed);
    }

    const std::string_view message = callbackData != nullptr && callbackData->pMessage != nullptr ? std::string_view(callbackData->pMessage) :
                                                                                                    std::string_view("(no Vulkan diagnostic text)");

    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        ZHLN::LogError("[Vulkan] {}", message);
        constexpr std::array<std::string_view, 4> k_gpu_bounds_markers = {"out of bounds", "Out of bounds", "OOB", "bounds check failed"};
        if (ContainsAny(message, k_gpu_bounds_markers)) {
            ZHLN::LogError("[Vulkan] GPU-assisted validation detected an out-of-bounds shader access; aborting.");
            std::abort();
        }
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        ZHLN::LogWarning("[Vulkan] {}", message);
    } else {
        ZHLN::Log("[Vulkan] {}", message);
    }

    return VK_FALSE;
}

auto Instance::Create(
    const std::string_view                  appName,
    const uint32_t                          appVersion,
    const std::span<const std::string_view> extensions,
    const ValidationMode                    validation
) noexcept -> std::expected<Instance, Vk::Error> {
    Instance result;
    // Telemetry is set up here, decoupled from construction: the constructor
    // cannot report failure, Create can.
    result._debugState.reset(new (std::nothrow) DebugState {});
    if (result._debugState == nullptr) {
        return std::unexpected(Vk::Error {VK_ERROR_OUT_OF_HOST_MEMORY});
    }
    result.RebindDebugState();

    const DiagnosticsSink sink = s_registered_sink.load(std::memory_order::acquire);
    if (sink.Valid()) {
        result._validationTarget = sink.validation;
        result._deviceLostTarget = sink.deviceLost;
    }

    if (auto res = volkInitialize(); res != VK_SUCCESS) {
        return std::unexpected(Vk::Error {res});
    }

    const std::vector<VkExtensionProperties> available_extensions = EnumerateInstanceExtensions();
    const std::vector<VkLayerProperties>     available_layers     = EnumerateInstanceLayers();

    constexpr std::string_view k_validation_layer = "VK_LAYER_KHRONOS_validation";
    bool                       enable_validation  = validation != ValidationMode::Off;
    bool                       gpu_validation     = validation == ValidationMode::GPU;
    if (enable_validation && !HasLayer(available_layers, k_validation_layer)) {
        ZHLN::LogWarning(
            "[Vulkan] Validation layer {} is not available; continuing without validation. Install the Vulkan SDK or configure VK_LAYER_PATH to enable it.",
            k_validation_layer
        );
        enable_validation = false;
        gpu_validation    = false;
    }

    std::vector<std::string> enabled_extension_names;
    enabled_extension_names.reserve(extensions.size() + 3);
    const auto add_if_supported = [&](const std::string_view name, const std::string_view messagePrefix) {
        if (!HasExtension(available_extensions, name)) {
            ZHLN::LogWarning("{}{}", messagePrefix, name);
            return false;
        }
        if (!std::ranges::contains(enabled_extension_names, name)) {
            enabled_extension_names.emplace_back(name);
        }
        return true;
    };

    for (const std::string_view extension: extensions) {
        add_if_supported(extension, "[Vulkan] Skipping unsupported instance extension: ");
    }

    const bool debug_utils_enabled         = validation != ValidationMode::Off &&
                                             add_if_supported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, "[Vulkan] Debug utils is unavailable: ");
    const bool validation_features_enabled = enable_validation && gpu_validation &&
                                             add_if_supported(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, "[Vulkan] GPU-assisted validation is unavailable: ");
    if (gpu_validation && !validation_features_enabled) {
        gpu_validation = false;
    }
    const bool layer_settings_enabled = enable_validation && gpu_validation &&
                                        add_if_supported(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME, "[Vulkan] Layer settings extension is unavailable: ");

    std::vector<const char*> enabled_extensions;
    enabled_extensions.reserve(enabled_extension_names.size());
    for (const std::string& name: enabled_extension_names) {
        enabled_extensions.push_back(name.c_str());
    }

    std::string             app_name_storage(appName);
    const VkApplicationInfo application_info {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = app_name_storage.c_str(),
        .applicationVersion = appVersion,
        .apiVersion         = VK_API_VERSION_1_3,
    };

    std::array<VkValidationFeatureEnableEXT, 2> enabled_validation_features {};
    uint32_t                                    enabled_validation_feature_count = 0;
    if (gpu_validation && validation_features_enabled) {
        enabled_validation_features[enabled_validation_feature_count++] = VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT;
        enabled_validation_features[enabled_validation_feature_count++] = VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT;
    }

    VkValidationFeaturesEXT validation_features {
        .sType                          = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .pNext                          = nullptr,
        .enabledValidationFeatureCount  = enabled_validation_feature_count,
        .pEnabledValidationFeatures     = enabled_validation_feature_count != 0 ? enabled_validation_features.data() : nullptr,
        .disabledValidationFeatureCount = 0,
        .pDisabledValidationFeatures    = nullptr,
    };

    constexpr const char*                  k_validation_layer_name = "VK_LAYER_KHRONOS_validation";
    const VkBool32                         force_robustness        = VK_TRUE;
    const VkBool32                         dump_descriptors        = VK_TRUE;
    const VkBool32                         dump_to_stdout          = VK_TRUE;
    const std::array<VkLayerSettingEXT, 3> layer_settings          = {{
        {.pLayerName   = k_validation_layer_name,
         .pSettingName = "gpuav_force_on_robustness",
         .type         = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         .valueCount   = 1,
         .pValues      = &force_robustness},
        {.pLayerName   = k_validation_layer_name,
         .pSettingName = "gpu_dump_descriptors",
         .type         = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         .valueCount   = 1,
         .pValues      = &dump_descriptors},
        {.pLayerName   = k_validation_layer_name,
         .pSettingName = "gpu_dump_to_stdout",
         .type         = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         .valueCount   = 1,
         .pValues      = &dump_to_stdout},
    }};

    VkLayerSettingsCreateInfoEXT layer_settings_info {
        .sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        .pNext        = gpu_validation && validation_features_enabled ? &validation_features : nullptr,
        .settingCount = layer_settings_enabled ? static_cast<uint32_t>(layer_settings.size()) : 0U,
        .pSettings    = layer_settings_enabled ? layer_settings.data() : nullptr,
    };

    VkDebugUtilsMessengerCreateInfoEXT debug_info {
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext           = nullptr,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = &Instance::DebugCallback,
        .pUserData       = result._debugState.get(),
    };

    const void* validation_chain = nullptr;
    if (gpu_validation && validation_features_enabled) {
        validation_chain = layer_settings_enabled ? static_cast<const void*>(&layer_settings_info) : static_cast<const void*>(&validation_features);
    }
    debug_info.pNext = enable_validation ? validation_chain : nullptr;

    VkInstanceCreateInfo create_info {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = validation != ValidationMode::Off && debug_utils_enabled ? &debug_info : nullptr,
        .flags                   = 0,
        .pApplicationInfo        = &application_info,
        .enabledLayerCount       = enable_validation ? 1U : 0U,
        .ppEnabledLayerNames     = enable_validation ? &k_validation_layer_name : nullptr,
        .enabledExtensionCount   = static_cast<uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.empty() ? nullptr : enabled_extensions.data(),
    };

#if defined(__APPLE__)
    create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VkInstance handle = VK_NULL_HANDLE;

    if (const auto res = vkCreateInstance(&create_info, nullptr, &handle); res != VK_SUCCESS) {
        return std::unexpected(Vk::Error {res});
    }

    volkLoadInstance(handle);
    result._handle                        = handle;
    result._debugState->debugUtilsEnabled = debug_utils_enabled;

    if (validation != ValidationMode::Off && debug_utils_enabled && vkCreateDebugUtilsMessengerEXT != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = debug_info;
        messenger_info.pNext                              = nullptr;
        const VkResult messenger_created                  = vkCreateDebugUtilsMessengerEXT(handle, &messenger_info, nullptr, &result._messenger);
        if (messenger_created != VK_SUCCESS) {
            result._messenger = VK_NULL_HANDLE;
            ZHLN::LogWarning("[Vulkan] Could not create validation debug messenger: {}", static_cast<int32_t>(messenger_created));
        }

        if (vkCreateDebugUtilsMessengerEXT != nullptr) {
            const VkDebugUtilsMessengerCreateInfoEXT address_binding_info {
                .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .pNext           = nullptr,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
                .pfnUserCallback = &Instance::DebugCallback,
                .pUserData       = result._debugState.get(),
            };
            const VkResult address_messenger_created = vkCreateDebugUtilsMessengerEXT(handle, &address_binding_info, nullptr, &result._addressBindingMessenger);
            if (address_messenger_created != VK_SUCCESS) {
                result._addressBindingMessenger = VK_NULL_HANDLE;
            }
        }
    }

    Instance* expected = nullptr;
    if (!s_active.compare_exchange_strong(expected, &result, std::memory_order::release, std::memory_order::relaxed)) {
        ZHLN::LogError("[Vulkan] Only one active Vulkan instance is supported by the diagnostics bridge.");
        result.Destroy();
        return std::unexpected(Vk::Error {VK_ERROR_INITIALIZATION_FAILED}); // This is why I fucking hate statics.
    }

    return result;
}

auto InstanceBuilder::Build() noexcept -> std::expected<Instance, Vk::Error> {
    return Vk::Instance::Create(_appName, _appVersion, _extensions, _validationMode);
}

auto Instance::ValidationErrorCount() noexcept -> uint32_t {
    const Instance* const active = s_active.load(std::memory_order::acquire);
    return active != nullptr ? active->_validationTarget->load(std::memory_order::relaxed) : 0;
}

auto Instance::DeviceLostCount() noexcept -> uint32_t {
    const Instance* const active = s_active.load(std::memory_order::acquire);
    return active != nullptr ? active->_deviceLostTarget->load(std::memory_order::relaxed) : 0;
}

void Instance::IncrementNumericalDeviceLoss() noexcept {
    if (Instance* const active = s_active.load(std::memory_order::acquire); active != nullptr) {
        active->_deviceLostTarget->fetch_add(1, std::memory_order::relaxed);
    }
}

} // namespace ZHLN::Vk
