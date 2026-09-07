// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/FileSystemWatcher.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace ZHLN {
namespace {

namespace fs      = std::filesystem;
using SteadyClock = std::chrono::steady_clock;
using FileTime    = fs::file_time_type;

constexpr auto kPollInterval = std::chrono::milliseconds {25};

enum class SettleState : uint8_t {
    Idle,
    Settling,
};

struct ObservedEntry {
    fs::path path;
    FileTime mtime;
};

struct ScanResult {
    /// False means some paths could not be inspected. Existing entries absent
    /// from this partial snapshot must not be mistaken for deletions.
    bool                                 complete = true;
    std::map<std::string, ObservedEntry> entries;
};

struct TrackedEntry {
    fs::path                path;
    FileTime                lastMtime;
    SteadyClock::time_point lastChange;
    SettleState             settleState     = SettleState::Idle;
    FileWatchAction         pendingAction   = FileWatchAction::Modified;
    bool                    exists          = false;
    bool                    deliveredExists = false;
};

[[nodiscard]] auto PathKey(const fs::path& path) -> std::string {
    return path.lexically_normal().generic_string();
}

[[nodiscard]] auto MatchesFilter(const fs::path& path, const WatchDescriptor& descriptor) -> bool {
    return descriptor.extensionFilter.empty() || path.extension() == descriptor.extensionFilter;
}

void AddObservedFile(ScanResult& result, const fs::path& path, const WatchDescriptor& descriptor) {
    if (!MatchesFilter(path, descriptor)) {
        return;
    }

    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        if (ec) {
            result.complete = false;
        }
        return;
    }

    const FileTime mtime = fs::last_write_time(path, ec);
    if (ec) {
        result.complete = false;
        return;
    }

    fs::path normalized = path.lexically_normal();
    result.entries.emplace(PathKey(normalized), ObservedEntry {.path = std::move(normalized), .mtime = mtime});
}

[[nodiscard]] auto Scan(const WatchDescriptor& descriptor) -> ScanResult {
    ScanResult      result;
    std::error_code ec;
    const bool      exists = fs::exists(descriptor.path, ec);
    if (ec) {
        result.complete = false;
        return result;
    }
    if (!exists) {
        return result;
    }

    if (!descriptor.isDirectory) {
        AddObservedFile(result, descriptor.path, descriptor);
        return result;
    }

    if (!fs::is_directory(descriptor.path, ec) || ec) {
        result.complete = false;
        return result;
    }

    constexpr auto options = fs::directory_options::skip_permission_denied;
    if (descriptor.recursive) {
        fs::recursive_directory_iterator       it {descriptor.path, options, ec};
        const fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            AddObservedFile(result, it->path(), descriptor);
            it.increment(ec);
        }
    } else {
        fs::directory_iterator       it {descriptor.path, options, ec};
        const fs::directory_iterator end;
        while (!ec && it != end) {
            AddObservedFile(result, it->path(), descriptor);
            it.increment(ec);
        }
    }

    if (ec) {
        result.complete = false;
    }
    return result;
}

void BeginSettling(TrackedEntry& entry, FileWatchAction action, SteadyClock::time_point now) {
    entry.pendingAction = action;
    entry.lastChange    = now;
    entry.settleState   = SettleState::Settling;
}

} // namespace

struct FileSystemWatcher::Impl {
    struct Subscription {
        WatchDescriptor   descriptor;
        FileWatchCallback callback;
    };

    struct SubscriptionSnapshot {
        FileWatchHandle handle = 0;
        WatchDescriptor descriptor;
    };

    struct WorkerWatch {
        WatchDescriptor                     descriptor;
        bool                                initialized = false;
        std::map<std::string, TrackedEntry> entries;
    };

    struct StagedEvent {
        FileWatchHandle handle = 0;
        FileWatchEvent  event;
    };

    std::atomic<bool> stop {false};
    std::thread       worker;
    std::thread::id   dispatchThread = std::this_thread::get_id();

    Mutex                                   subscriptionMutex {};
    std::map<FileWatchHandle, Subscription> subscriptions;
    FileWatchHandle                         nextHandle = 1;

    Mutex                    readyMutex {};
    std::vector<StagedEvent> readyEvents;

    [[nodiscard]] auto SnapshotSubscriptions() -> std::vector<SubscriptionSnapshot> {
        const std::lock_guard<Mutex>      lock(subscriptionMutex);
        std::vector<SubscriptionSnapshot> snapshots;
        snapshots.reserve(subscriptions.size());
        for (const auto& [handle, subscription]: subscriptions) {
            snapshots.push_back({.handle = handle, .descriptor = subscription.descriptor});
        }
        return snapshots;
    }

    void Stage(std::vector<StagedEvent>& events) {
        if (events.empty()) {
            return;
        }

        const std::lock_guard<Mutex> lock(readyMutex);
        readyEvents.reserve(readyEvents.size() + events.size());
        for (StagedEvent& event: events) {
            readyEvents.push_back(std::move(event));
        }
    }

    static void PromoteSettled(FileWatchHandle handle, WorkerWatch& watch, SteadyClock::time_point now, std::vector<StagedEvent>& ready) {
        const auto settleWindow = std::chrono::milliseconds {watch.descriptor.debounceMs};
        for (auto& [key, entry]: watch.entries) {
            static_cast<void>(key);
            if (entry.settleState != SettleState::Settling || now - entry.lastChange < settleWindow) {
                continue;
            }

            ready.push_back({.handle = handle, .event = {.path = entry.path, .action = entry.pendingAction}});
            entry.settleState     = SettleState::Idle;
            entry.deliveredExists = entry.pendingAction != FileWatchAction::Deleted;
        }
    }

    static void Observe(FileWatchHandle handle, WorkerWatch& watch, const ScanResult& scan, SteadyClock::time_point now, std::vector<StagedEvent>& ready) {
        if (!watch.initialized) {
            // Do not establish a baseline from an incomplete scan: a locked or
            // inaccessible directory must not turn its pre-existing content
            // into synthetic Created events once it becomes readable.
            if (!scan.complete) {
                return;
            }

            // Existing content is the subscription baseline. A later file
            // creation is still observed because a missing target produces an
            // empty baseline on this first scan.
            for (const auto& [key, observed]: scan.entries) {
                watch.entries.emplace(
                    key, TrackedEntry {
                             .path            = observed.path,
                             .lastMtime       = observed.mtime,
                             .settleState     = SettleState::Idle,
                             .exists          = true,
                             .deliveredExists = true,
                         }
                );
            }
            watch.initialized = true;
            return;
        }

        for (const auto& [key, observed]: scan.entries) {
            auto [it, inserted] = watch.entries.try_emplace(key);
            TrackedEntry& entry = it->second;
            if (inserted) {
                entry.path      = observed.path;
                entry.lastMtime = observed.mtime;
                entry.exists    = true;
                BeginSettling(entry, FileWatchAction::Created, now);
                continue;
            }

            entry.path = observed.path;
            if (!entry.exists) {
                entry.exists    = true;
                entry.lastMtime = observed.mtime;
                BeginSettling(entry, entry.deliveredExists ? FileWatchAction::Modified : FileWatchAction::Created, now);
                continue;
            }

            if (entry.lastMtime != observed.mtime) {
                entry.lastMtime = observed.mtime;
                BeginSettling(entry, entry.deliveredExists ? FileWatchAction::Modified : FileWatchAction::Created, now);
            }
        }

        // A partial scan cannot prove a path was removed. This protects a
        // subscription from reporting a deletion while a tool holds a file or
        // the directory is temporarily inaccessible.
        if (scan.complete) {
            for (auto& [key, entry]: watch.entries) {
                if (scan.entries.contains(key) || !entry.exists) {
                    continue;
                }

                entry.exists = false;
                if (!entry.deliveredExists) {
                    // A new file disappeared before its creation settled, so
                    // it was never observable by subscribers at all.
                    entry.settleState = SettleState::Idle;
                } else {
                    BeginSettling(entry, FileWatchAction::Deleted, now);
                }
            }
        }

        PromoteSettled(handle, watch, now, ready);
    }

    void Run() {
        std::map<FileWatchHandle, WorkerWatch> watches;
        while (!stop.load(std::memory_order_acquire)) {
            const std::vector<SubscriptionSnapshot> snapshots = SnapshotSubscriptions();
            std::set<FileWatchHandle>               activeHandles;
            std::vector<StagedEvent>                ready;
            const auto                              now = SteadyClock::now();

            for (const SubscriptionSnapshot& snapshot: snapshots) {
                activeHandles.insert(snapshot.handle);
                auto [it, inserted] = watches.try_emplace(snapshot.handle);
                WorkerWatch& watch  = it->second;
                if (inserted) {
                    watch.descriptor = snapshot.descriptor;
                }
                Observe(snapshot.handle, watch, Scan(watch.descriptor), now, ready);
            }

            for (auto it = watches.begin(); it != watches.end();) {
                if (!activeHandles.contains(it->first)) {
                    it = watches.erase(it);
                } else {
                    ++it;
                }
            }

            Stage(ready);
            std::this_thread::sleep_for(kPollInterval);
        }
    }
};

FileSystemWatcher::FileSystemWatcher(): _impl(std::make_unique<Impl>()) {
    _impl->worker = std::thread(&Impl::Run, _impl.get());
}

FileSystemWatcher::~FileSystemWatcher() {
    if (_impl == nullptr) {
        return;
    }

    _impl->stop.store(true, std::memory_order_release);
    if (_impl->worker.joinable()) {
        _impl->worker.join();
    }
}

auto FileSystemWatcher::Watch(WatchDescriptor descriptor, FileWatchCallback callback) -> FileWatchHandle {
    if (descriptor.path.empty() || !callback) {
        return 0;
    }

    descriptor.path = descriptor.path.lexically_normal();
    const std::lock_guard<Mutex> lock(_impl->subscriptionMutex);
    const FileWatchHandle        handle = _impl->nextHandle++;
    _impl->subscriptions.emplace(handle, Impl::Subscription {.descriptor = std::move(descriptor), .callback = std::move(callback)});
    return handle;
}

auto FileSystemWatcher::WatchFile(std::filesystem::path path, FileWatchCallback callback, uint32_t debounceMs) -> FileWatchHandle {
    return Watch({.path = std::move(path), .isDirectory = false, .recursive = false, .extensionFilter = {}, .debounceMs = debounceMs}, std::move(callback));
}

auto FileSystemWatcher::WatchDirectory(
    std::filesystem::path directory,
    FileWatchCallback     callback,
    bool                  recursive,
    std::string           extensionFilter,
    uint32_t              debounceMs
) -> FileWatchHandle {
    return Watch(
        {.path = std::move(directory), .isDirectory = true, .recursive = recursive, .extensionFilter = std::move(extensionFilter), .debounceMs = debounceMs},
        std::move(callback)
    );
}

auto FileSystemWatcher::Unwatch(FileWatchHandle handle) -> bool {
    if (handle == 0) {
        return false;
    }

    const std::lock_guard<Mutex> lock(_impl->subscriptionMutex);
    return _impl->subscriptions.erase(handle) != 0;
}

void FileSystemWatcher::DispatchEvents() {
    if (std::this_thread::get_id() != _impl->dispatchThread) {
        ZHLN::Log("[FileSystemWatcher] DispatchEvents() ignored outside its owner thread.");
        return;
    }

    std::vector<Impl::StagedEvent> ready;
    {
        const std::lock_guard<Mutex> lock(_impl->readyMutex);
        ready.swap(_impl->readyEvents);
    }

    for (const Impl::StagedEvent& staged: ready) {
        FileWatchCallback callback;
        {
            const std::lock_guard<Mutex> lock(_impl->subscriptionMutex);
            const auto                   it = _impl->subscriptions.find(staged.handle);
            if (it == _impl->subscriptions.end()) {
                continue;
            }
            callback = it->second.callback;
        }

        if (callback) {
            callback(staged.event);
        }
    }
}

} // namespace ZHLN
