// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Extensions.hpp"

#include <Zahlen/Log.hpp>
#include "RenderCore.h"

namespace ZHLN::Vk {

enum class ExtensionBuilderError : uint8_t {
    MissingRequiredExtension ZHLN_ANNOTATION(ZHLN::Description<"A required Vulkan extension is missing">{}) = 1,
};


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


ExtensionBuilder::ExtensionBuilder(std::vector<std::string>&& available) noexcept: _available(std::move(available)) {
}

auto ExtensionBuilder::ForDevice(VkPhysicalDevice physical) noexcept -> ExtensionBuilder {
    return ExtensionBuilder(TemplatedDetail::ExtensionNames(EnumerateDeviceExtensions(physical)));
}

auto ExtensionBuilder::ForInstance() noexcept -> ExtensionBuilder {
    return ExtensionBuilder(TemplatedDetail::ExtensionNames(EnumerateInstanceExtensions()));
}

auto ExtensionBuilder::ForAvailable(std::span<const std::string_view> names) -> ExtensionBuilder {
    std::vector<std::string> available;
    available.reserve(names.size());
    for (const auto name: names) {
        available.emplace_back(name);
    }
    return ExtensionBuilder(std::move(available));
}

bool ExtensionBuilder::Supports(std::string_view name) const noexcept { return IsSupported(name); }

bool ExtensionBuilder::SupportsAll(std::initializer_list<std::string_view> names) const noexcept {
    return std::ranges::all_of(names, [this](std::string_view name) { return IsSupported(name); });
}

auto ExtensionBuilder::Require(std::string_view name) noexcept -> ExtensionBuilder& {
    if (Supports(name)) {
        Optional(name);
    } else {
        _missingRequired.emplace_back(name);
    }
    return *this;
}

auto ExtensionBuilder::Optional(std::string_view name) noexcept -> ExtensionBuilder& {
    if (const auto* matched = FindAvailable(name); matched != nullptr && !std::ranges::contains(_active, *matched)) {
        _active.push_back(*matched);
    }
    return *this;
}

auto ExtensionBuilder::OptionalGroup(std::initializer_list<std::string_view> names, bool condition) noexcept -> ExtensionBuilder& {
    if (condition && SupportsAll(names)) {
        for (auto name: names) {
            Optional(name);
        }
    }
    return *this;
}

auto ExtensionBuilder::Build() noexcept -> std::expected<ExtensionResult, ZHLN::ErrorCode> {
    if (!_missingRequired.empty()) {
        for (const auto& name: _missingRequired) {
            ZHLN::Log("[Extensions] Required Vulkan extension not supported by this driver: {}", name);
        }
        return std::unexpected(ExtensionBuilderError::MissingRequiredExtension);
    }
    return ExtensionResult(std::move(_active));
}

[[nodiscard]] bool ExtensionBuilder::IsSupported(std::string_view name) const noexcept {
    return std::ranges::contains(_available, name);
}

auto ExtensionBuilder::FindAvailable(std::string_view name) const noexcept -> const std::string* {
    auto it = std::ranges::find(_available, name);
    return it != _available.end() ? &(*it) : nullptr;
}

}
