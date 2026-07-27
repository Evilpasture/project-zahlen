// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Log.hpp>
#include <filesystem>
#include <system_error> // Required for std::error_code

namespace ZHLN {
class FileWatcher {
  public:
    explicit FileWatcher(std::string path): _path(std::move(path)) {
        std::error_code ec;
        _lastWriteTime = std::filesystem::last_write_time(_path, ec);
        if (ec) {
            // If the file doesn't exist yet, we initialize to zero
            _lastWriteTime = std::filesystem::file_time_type::min();
        }
    }

    bool CheckModified() {
        std::error_code ec;
        auto            currentWriteTime = std::filesystem::last_write_time(_path, ec);

        if (ec) {
            if (!std::filesystem::exists(_path)) {
                return false; // Silently wait for the file to be generated
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
