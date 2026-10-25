// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <memory>
#include <source_location>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

// Type-erased, owning container for optional domain services (e.g.
// AnimationContext). Kept reflection-free so headers included inside C++20
// module global fragments never pull std::meta across module boundaries.
class ExtensionServices {
  public:
    ExtensionServices()                                    = default;
    ~ExtensionServices() noexcept                          = default;
    ExtensionServices(const ExtensionServices&)            = delete;
    ExtensionServices& operator=(const ExtensionServices&) = delete;
    ExtensionServices(ExtensionServices&&) noexcept        = default;
    ExtensionServices& operator=(ExtensionServices&&) noexcept = default;

    template <typename T, typename... Args>
    auto Emplace(Args&&... args) -> std::remove_cv_t<T>& {
        using Clean       = std::remove_cv_t<T>;
        const uint64_t id = TypeKey<Clean>();
        for (Entry& entry: _entries) {
            if (entry.typeId == id) {
                auto replacement = std::unique_ptr<void, Deleter>(new Clean(std::forward<Args>(args)...), &Destroy<Clean>);
                entry.instance   = std::move(replacement);
                return *static_cast<Clean*>(entry.instance.get());
            }
        }
        auto instance = std::unique_ptr<void, Deleter>(new Clean(std::forward<Args>(args)...), &Destroy<Clean>);
        auto* raw     = static_cast<Clean*>(instance.get());
        _entries.push_back(Entry {.typeId = id, .instance = std::move(instance)});
        return *raw;
    }

    template <typename T>
    [[nodiscard]] auto Get() noexcept -> ZHLN::Optional<std::remove_cv_t<T>&> {
        using Clean       = std::remove_cv_t<T>;
        const uint64_t id = TypeKey<Clean>();
        for (const Entry& entry: _entries) {
            if (entry.typeId == id && entry.instance != nullptr) {
                return *static_cast<Clean*>(entry.instance.get());
            }
        }
        return std::nullopt;
    }

    template <typename T>
    [[nodiscard]] auto Get() const noexcept -> ZHLN::Optional<const std::remove_cv_t<T>&> {
        using Clean       = std::remove_cv_t<T>;
        const uint64_t id = TypeKey<Clean>();
        for (const Entry& entry: _entries) {
            if (entry.typeId == id && entry.instance != nullptr) {
                return *static_cast<const Clean*>(entry.instance.get());
            }
        }
        return std::nullopt;
    }

    void Clear() noexcept {
        while (!_entries.empty()) {
            _entries.pop_back();
        }
    }

  private:
    using Deleter = void (*)(void*) noexcept;

    template <typename Clean>
    [[nodiscard]] static consteval auto TypeKey() noexcept -> uint64_t {
        return Hash64(std::source_location::current().function_name());
    }

    template <typename Clean>
    static void Destroy(void* ptr) noexcept {
        delete static_cast<Clean*>(ptr);
    }

    struct Entry {
        uint64_t                       typeId = 0;
        std::unique_ptr<void, Deleter> instance {nullptr, +[](void*) noexcept {}};
    };

    std::vector<Entry> _entries;
};

} // namespace ZHLN::ECS
