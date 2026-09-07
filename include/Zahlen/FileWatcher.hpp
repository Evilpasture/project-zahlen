// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Log.hpp>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace ZHLN {

/// Small polling watcher for optional hot-reload clients. It depends only on
/// the public filesystem/logging surface, so renderer and scripting code do
/// not need an engine-private header to watch their own assets.
class FileWatcher {
  public:
    explicit FileWatcher(std::string path): _path(std::move(path)) {
        std::error_code ec;
        _lastWriteTime = std::filesystem::last_write_time(_path, ec);
        if (ec) {
            // A generated asset may not exist when the watcher is registered.
            _lastWriteTime = std::filesystem::file_time_type::min();
        }
    }

    [[nodiscard]] bool CheckModified() {
        std::error_code ec;
        auto            currentWriteTime = std::filesystem::last_write_time(_path, ec);

        if (ec) {
            if (!std::filesystem::exists(_path)) {
                return false;
            }
            ZHLN::Log("Script file is locked. File path: {}", _path);
            return false;
        }

        if (currentWriteTime != _lastWriteTime) {
            _lastWriteTime = currentWriteTime;
            return true;
        }

        return false;
    }

  private:
    std::string                     _path;
    std::filesystem::file_time_type _lastWriteTime;
};

} // namespace ZHLN
