// src/render/Extensions.hpp
#pragma once

namespace ZHLN::Vk {

class ExtensionResult {
  public:
    ExtensionResult() = default;

    explicit ExtensionResult(std::vector<std::string>&& strings) noexcept: _strings(std::move(strings)) {
        RebuildPointers();
    }

    // Disable copying to avoid expensive overhead and pointer invalidation
    ExtensionResult(const ExtensionResult&)                    = delete;
    auto operator=(const ExtensionResult&) -> ExtensionResult& = delete;

    // Support moving safely by rebuilding internal pointers
    ExtensionResult(ExtensionResult&& other) noexcept: _strings(std::move(other._strings)) {
        RebuildPointers();
    }

    auto operator=(ExtensionResult&& other) noexcept -> ExtensionResult& {
        if (this != &other) {
            _strings = std::move(other._strings);
            RebuildPointers();
        }
        return *this;
    }

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
    void RebuildPointers() noexcept {
        _ptrs.clear();
        _ptrs.reserve(_strings.size());
        _views.clear();
        _views.reserve(_strings.size());
        for (const auto& s: _strings) {
            _ptrs.push_back(s.c_str());
            _views.emplace_back(s);
        }
    }

    std::vector<std::string>      _strings;
    std::vector<const char*>      _ptrs;
    std::vector<std::string_view> _views;
};

class ExtensionBuilder {
  public:
    ExtensionBuilder() = default;

    // Factory for Device Extensions
    [[nodiscard]] static auto ForDevice(VkPhysicalDevice physical) noexcept -> ExtensionBuilder {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> props(count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, props.data());

        std::vector<std::string> available;
        available.reserve(count);
        for (const auto& p: props) {
            available.emplace_back(p.extensionName);
        }
        return ExtensionBuilder(std::move(available));
    }

    // Factory for Instance Extensions
    [[nodiscard]] static auto ForInstance() noexcept -> ExtensionBuilder {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> props(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());

        std::vector<std::string> available;
        available.reserve(count);
        for (const auto& p: props) {
            available.emplace_back(p.extensionName);
        }
        return ExtensionBuilder(std::move(available));
    }

    // --- Fluent Chain Interface ---

    auto Require(std::string_view name) noexcept -> ExtensionBuilder& {
        if (IsSupported(name)) {
            if (const auto* matched = FindAvailable(name)) {
                _active.push_back(*matched);
            }
        } else {
            _missingRequired.emplace_back(name);
        }
        return *this;
    }

    auto RequireIf(std::string_view name, bool condition) noexcept -> ExtensionBuilder& {
        if (condition) {
            return Require(name);
        }
        return *this;
    }

    auto Optional(std::string_view name) noexcept -> ExtensionBuilder& {
        if (IsSupported(name)) {
            if (const auto* matched = FindAvailable(name)) {
                _active.push_back(*matched);
            }
        }
        return *this;
    }

    auto OptionalIf(std::string_view name, bool condition) noexcept -> ExtensionBuilder& {
        if (condition) {
            return Optional(name);
        }
        return *this;
    }

    auto OptionalGroup(std::initializer_list<std::string_view> names, bool condition = true) noexcept -> ExtensionBuilder& {
        if (!condition) {
            return *this;
        }

        bool all_supported = true;
        for (auto name: names) {
            if (!IsSupported(name)) {
                all_supported = false;
                break;
            }
        }

        if (all_supported) {
            for (auto name: names) {
                if (const auto* matched = FindAvailable(name)) {
                    _active.push_back(*matched);
                }
            }
        }
        return *this;
    }

    auto Debug(bool enable) noexcept -> ExtensionBuilder& {
        return OptionalIf(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, enable);
    }

    // --- Output compilation ---

    [[nodiscard]] auto Build() noexcept -> std::expected<ExtensionResult, std::string> {
        if (!_missingRequired.empty()) {
            std::string error_msg = "Missing required extension(s):";
            for (const auto& missing: _missingRequired) {
                error_msg += " " + missing;
            }
            return std::unexpected(error_msg);
        }
        return ExtensionResult(std::move(_active));
    }

  private:
    explicit ExtensionBuilder(std::vector<std::string>&& available) noexcept: _available(std::move(available)) {
    }

    [[nodiscard]] bool IsSupported(std::string_view name) const noexcept {
        return std::ranges::any_of(_available, [name](const std::string& avail) { return avail == name; });
    }

    [[nodiscard]] auto FindAvailable(std::string_view name) const noexcept -> const std::string* {
        auto it = std::ranges::find_if(_available, [name](const std::string& avail) { return avail == name; });
        return it != _available.end() ? &(*it) : nullptr;
    }

    std::vector<std::string> _available;
    std::vector<std::string> _active;
    std::vector<std::string> _missingRequired;
};

} // namespace ZHLN::Vk
