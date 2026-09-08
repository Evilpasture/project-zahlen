// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FileSystemWatcher.hpp
//
// Engine-owned filesystem observation. Filesystem probing runs on a dedicated
// worker; subscriber callbacks are invoked only by DispatchEvents() on the
// thread that created the watcher (the engine main thread).
#pragma once

#include <Zahlen/Common.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ZHLN {

enum class FileWatchAction : uint8_t {
    Created,
    Modified,
    Deleted,
};

/// Immutable subscription configuration copied by FileSystemWatcher::Watch.
/// An empty extensionFilter accepts every regular file in a directory watch.
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

/// A polling filesystem observer with main-thread callback dispatch.
///
/// Registrations establish an initial baseline without emitting synthetic
/// Created events. Thereafter a change must remain stable for the descriptor's
/// debounce window before it is staged for DispatchEvents().
class ZHLN_API FileSystemWatcher {
  public:
    static constexpr uint32_t kDefaultDebounceMs = 100;

    FileSystemWatcher();
    ~FileSystemWatcher();

    FileSystemWatcher(const FileSystemWatcher&)                    = delete;
    auto operator=(const FileSystemWatcher&) -> FileSystemWatcher& = delete;
    FileSystemWatcher(FileSystemWatcher&&)                         = delete;
    auto operator=(FileSystemWatcher&&) -> FileSystemWatcher&      = delete;

    /// Registers an arbitrary file or directory descriptor. Returns zero when
    /// the descriptor path or callback is empty.
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

    /// Prevents future dispatch for the subscription. Events already staged for
    /// this handle are discarded when DispatchEvents() reaches them.
    [[nodiscard]] auto Unwatch(FileWatchHandle handle) -> bool;

    /// Drains the worker's staged events and invokes subscriber callbacks.
    /// This must be called only from the creator thread; Engine calls it during
    /// FramePhase::HotReload before simulation and rendering work.
    void DispatchEvents();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
