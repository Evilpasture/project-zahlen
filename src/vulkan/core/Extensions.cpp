// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Extensions.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

enum class ExtensionBuilderError : uint8_t {
    MissingRequiredExtension ZHLN_ANNOTATION(ZHLN::Description<"A required Vulkan extension is missing">{}) = 1,
};

namespace {

template <typename Enumerate>
[[nodiscard]] auto EnumerateProperties(Enumerate&& enumerate) noexcept -> std::vector<VkExtensionProperties> {
    std::vector<VkExtensionProperties> available;
    for (;;) {
        uint32_t count = 0;
        if (enumerate(&count, nullptr) != VK_SUCCESS || count == 0) {
            return {};
        }

        available.resize(count);
        const VkResult result = enumerate(&count, available.data());
        if (result == VK_SUCCESS) {
            available.resize(count);
            return available;
        }
        if (result != VK_INCOMPLETE) {
            return {};
        }
    }
}

[[nodiscard]] auto ExtensionNames(const std::vector<VkExtensionProperties>& props) -> std::vector<std::string> {
    std::vector<std::string> names;
    names.reserve(props.size());
    for (const auto& prop: props) {
        names.emplace_back(prop.extensionName);
    }
    return names;
}

} // namespace

auto EnumerateInstanceExtensions() noexcept -> std::vector<VkExtensionProperties> {
    if (volkInitialize() != VK_SUCCESS || vkEnumerateInstanceExtensionProperties == nullptr) {
        return {};
    }
    return EnumerateProperties([](uint32_t* count, VkExtensionProperties* props) {
        return vkEnumerateInstanceExtensionProperties(nullptr, count, props);
    });
}

auto EnumerateDeviceExtensions(const VkPhysicalDevice physical) noexcept -> std::vector<VkExtensionProperties> {
    if (physical == VK_NULL_HANDLE || vkEnumerateDeviceExtensionProperties == nullptr) {
        return {};
    }
    return EnumerateProperties([physical](uint32_t* count, VkExtensionProperties* props) {
        return vkEnumerateDeviceExtensionProperties(physical, nullptr, count, props);
    });
}

auto HasExtension(const std::span<const VkExtensionProperties> available, const std::string_view name) noexcept -> bool {
    return std::ranges::any_of(available, [name](const VkExtensionProperties& prop) {
        return name == prop.extensionName;
    });
}

auto IsInstanceExtensionSupported(const std::string_view extension) noexcept -> bool {
    return HasExtension(EnumerateInstanceExtensions(), extension);
}

auto IsDeviceExtensionSupported(const VkPhysicalDevice physical, const std::string_view extension) noexcept -> bool {
    return HasExtension(EnumerateDeviceExtensions(physical), extension);
}

ExtensionResult::ExtensionResult(std::vector<std::string>&& strings) noexcept: _strings(std::move(strings)) {
    RebuildPointers();
}

ExtensionResult::ExtensionResult(ExtensionResult&& other) noexcept: _strings(std::move(other._strings)) {
    RebuildPointers();
}

auto ExtensionResult::operator=(ExtensionResult&& other) noexcept -> ExtensionResult& {
    if (this != &other) {
        _strings = std::move(other._strings);
        RebuildPointers();
    }
    return *this;
}

void ExtensionResult::RebuildPointers() noexcept {
    _ptrs.clear();
    _ptrs.reserve(_strings.size());
    _views.clear();
    _views.reserve(_strings.size());
    for (const auto& s: _strings) {
        _ptrs.push_back(s.c_str());
        _views.emplace_back(s);
    }
}

ExtensionBuilder::ExtensionBuilder(std::vector<std::string>&& available) noexcept: _available(std::move(available)) {}

auto ExtensionBuilder::ForDevice(const VkPhysicalDevice physical) noexcept -> ExtensionBuilder {
    return ExtensionBuilder(ExtensionNames(EnumerateDeviceExtensions(physical)));
}

auto ExtensionBuilder::ForInstance() noexcept -> ExtensionBuilder {
    return ExtensionBuilder(ExtensionNames(EnumerateInstanceExtensions()));
}

bool ExtensionBuilder::Supports(const std::string_view name) const noexcept {
    return IsSupported(name);
}

bool ExtensionBuilder::SupportsAll(const std::initializer_list<std::string_view> names) const noexcept {
    return std::ranges::all_of(names, [this](const std::string_view name) { return IsSupported(name); });
}

auto ExtensionBuilder::Require(const std::string_view name) noexcept -> ExtensionBuilder& {
    if (Supports(name)) {
        Optional(name);
    } else {
        _missingRequired.emplace_back(name);
    }
    return *this;
}

auto ExtensionBuilder::Optional(const std::string_view name) noexcept -> ExtensionBuilder& {
    if (const auto* matched = FindAvailable(name); matched != nullptr && !std::ranges::contains(_active, *matched)) {
        _active.push_back(*matched);
    }
    return *this;
}

auto ExtensionBuilder::OptionalGroup(const std::initializer_list<std::string_view> names, const bool condition) noexcept -> ExtensionBuilder& {
    if (condition && SupportsAll(names)) {
        for (const auto name: names) {
            Optional(name);
        }
    }
    return *this;
}

auto ExtensionBuilder::Build() noexcept -> std::expected<ExtensionResult, ErrorCode> {
    if (!_missingRequired.empty()) {
        for (const auto& name: _missingRequired) {
            ZHLN::LogError("[Vulkan] Required extension is not supported: {}", name);
        }
        return std::unexpected(ExtensionBuilderError::MissingRequiredExtension);
    }
    return ExtensionResult(std::move(_active));
}

bool ExtensionBuilder::IsSupported(const std::string_view name) const noexcept {
    return std::ranges::contains(_available, name);
}

auto ExtensionBuilder::FindAvailable(const std::string_view name) const noexcept -> const std::string* {
    const auto it = std::ranges::find(_available, name);
    return it != _available.end() ? &*it : nullptr;
}

} // namespace ZHLN::Vk
