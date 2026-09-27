// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Zahlen/Config.hpp>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

class ShaderReloadRegistry {
  public:
    void Register(std::string_view name, const std::vector<const char*>& paths, std::function<void()> callback);
    void Register(std::string_view name, std::initializer_list<const char*> paths, std::function<void()> callback) {
        Register(name, std::vector<const char*> {paths}, std::move(callback));
    }

    void Dispatch(const std::string& changedPath, const std::function<void()>& onFirstMatch);

    [[nodiscard]] auto Size() const noexcept -> size_t { return _entries.size(); }

    void Clear() noexcept { _entries.clear(); }

  private:
    struct Entry {
        std::string              name;
        std::vector<std::string> paths;
        std::function<void()>    callback;
    };

    std::vector<Entry> _entries;
};

}
