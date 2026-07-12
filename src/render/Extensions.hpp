// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Extensions.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other render headers."
#endif
#include <Zahlen/Error.hpp>
namespace ZHLN::Vk {

class ExtensionResult {
  public:
    ExtensionResult() = default;
    explicit ExtensionResult(std::vector<std::string>&& strings) noexcept;

    // Disable copying to avoid expensive overhead and pointer invalidation
    ExtensionResult(const ExtensionResult&)                    = delete;
    auto operator=(const ExtensionResult&) -> ExtensionResult& = delete;

    // Support moving safely by rebuilding internal pointers
    ExtensionResult(ExtensionResult&& other) noexcept;
    auto operator=(ExtensionResult&& other) noexcept -> ExtensionResult&;

    // Implicit conversion to std::vector<const char*> reference for device extensions
    operator const std::vector<const char*>&() const noexcept {
        return _ptrs;
    }

    // Implicit conversion to std::span<const std::string_view> for instance extensions
    operator std::span<const std::string_view>() const noexcept {
        return {_views.data(), _views.size()};
    }

    [[nodiscard]] auto data() const noexcept -> const char* const* {
        return _ptrs.data();
    }

    [[nodiscard]] auto size() const noexcept -> size_t {
        return _ptrs.size();
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return _ptrs.empty();
    }

  private:
    void RebuildPointers() noexcept;

    std::vector<std::string>      _strings;
    std::vector<const char*>      _ptrs;
    std::vector<std::string_view> _views;
};

class ExtensionBuilder {
  public:
    ExtensionBuilder() = default;

    // Factories
    [[nodiscard]] static auto ForDevice(VkPhysicalDevice physical) noexcept -> ExtensionBuilder;
    [[nodiscard]] static auto ForInstance() noexcept -> ExtensionBuilder;

    // Fluent Builders
    auto Require(std::string_view name) noexcept -> ExtensionBuilder&;

    auto RequireIf(std::string_view name, bool condition) noexcept -> ExtensionBuilder& {
        return condition ? Require(name) : *this;
    }

    auto Optional(std::string_view name) noexcept -> ExtensionBuilder&;

    auto OptionalIf(std::string_view name, bool condition) noexcept -> ExtensionBuilder& {
        return condition ? Optional(name) : *this;
    }

    auto OptionalGroup(std::initializer_list<std::string_view> names, bool condition = true) noexcept -> ExtensionBuilder&;

    auto Debug(bool enable) noexcept -> ExtensionBuilder& {
        return OptionalIf(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, enable);
    }

    // Output compilation
    [[nodiscard]] auto Build() noexcept -> std::expected<ExtensionResult, ZHLN::Error>;

  private:
    explicit ExtensionBuilder(std::vector<std::string>&& available) noexcept;

    [[nodiscard]] bool IsSupported(std::string_view name) const noexcept;

    [[nodiscard]] auto FindAvailable(std::string_view name) const noexcept -> const std::string*;

    std::vector<std::string> _available;
    std::vector<std::string> _active;
    std::vector<std::string> _missingRequired;
};

} // namespace ZHLN::Vk
