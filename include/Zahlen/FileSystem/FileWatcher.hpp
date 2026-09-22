// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FileSystem/FileWatcher.hpp
//
// Low-level file watching — polling observer with main-thread dispatch.
// Moved from include/Zahlen/FileSystemWatcher.hpp to zahlen_filesystem.

#pragma once

#include <Zahlen/Common.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ZHLN::FS {

enum class FileWatchAction : uint8_t {
    Created,
    Modified,
    Deleted,
};

struct WatchDescriptor {
    std::filesystem::path path;
    bool                  isDirectory     = false;
    bool                  recursive       = false;
    std::string           extensionFilter;
    uint32_t              debounceMs      = 100;
};

struct FileWatchEvent {
    std::filesystem::path path;
    FileWatchAction       action = FileWatchAction::Modified;
};

using FileWatchHandle   = uint64_t;
using FileWatchCallback = std::function<void(const FileWatchEvent&)>;

class ZHLN_API FileSystemWatcher {
  public:
    static constexpr uint32_t kDefaultDebounceMs = 100;

    FileSystemWatcher();
    ~FileSystemWatcher();

    FileSystemWatcher(const FileSystemWatcher&)                    = delete;
    auto operator=(const FileSystemWatcher&) -> FileSystemWatcher& = delete;
    FileSystemWatcher(FileSystemWatcher&&)                         = delete;
    auto operator=(FileSystemWatcher&&) -> FileSystemWatcher&      = delete;

    [[nodiscard]] auto Watch(WatchDescriptor descriptor, FileWatchCallback callback) -> FileWatchHandle;

    [[nodiscard]] auto WatchFile(
        std::filesystem::path path, FileWatchCallback callback, uint32_t debounceMs = kDefaultDebounceMs
    ) -> FileWatchHandle;

    [[nodiscard]] auto WatchDirectory(
        std::filesystem::path directory,
        FileWatchCallback     callback,
        bool                  recursive = true,
        std::string           extensionFilter = {},
        uint32_t              debounceMs = kDefaultDebounceMs
    ) -> FileWatchHandle;

    [[nodiscard]] auto Unwatch(FileWatchHandle handle) -> bool;

    void DispatchEvents();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN::FS

// Back-compat: old ZHLN::FileSystemWatcher etc still work
namespace ZHLN {
using FileWatchAction   = FS::FileWatchAction;
using WatchDescriptor   = FS::WatchDescriptor;
using FileWatchEvent    = FS::FileWatchEvent;
using FileWatchHandle   = FS::FileWatchHandle;
using FileWatchCallback = FS::FileWatchCallback;
using FileSystemWatcher = FS::FileSystemWatcher;
} // namespace ZHLN
