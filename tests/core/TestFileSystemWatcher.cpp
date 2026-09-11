// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/FileSystemWatcher.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

enum class FileSystemWatcherTestError : uint8_t {
    TemporaryDirectoryFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the temporary filesystem watcher test directory."> {}) = 1,
    ExpectedEventNotObserved ZHLN_ANNOTATION(ZHLN::Description<"The filesystem watcher did not dispatch the expected settled event."> {}),
};

[[nodiscard]] auto UniqueTestDirectory(std::error_code& ec) -> fs::path {
    const fs::path temporary = fs::temp_directory_path(ec);
    if (ec) {
        return {};
    }
    return temporary / ("zahlen-file-system-watcher-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

[[nodiscard]] bool DispatchUntil(ZHLN::FileSystemWatcher& watcher, const std::vector<ZHLN::FileWatchEvent>& events, size_t expectedCount) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        watcher.DispatchEvents();
        if (events.size() >= expectedCount) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    watcher.DispatchEvents();
    return events.size() >= expectedCount;
}

struct FileSystemWatcherTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> file_lifecycle_is_debounced_and_dispatched_on_demand() {
            std::error_code ec;
            const fs::path root = UniqueTestDirectory(ec);
            if (!ZHLN::Test::ExpectTrue(!ec)) {
                return std::unexpected(FileSystemWatcherTestError::TemporaryDirectoryFailed);
            }
            const fs::path file = root / "generated.script";
            fs::create_directories(root, ec);
            if (!ZHLN::Test::ExpectTrue(!ec)) {
                return std::unexpected(FileSystemWatcherTestError::TemporaryDirectoryFailed);
            }

            std::vector<ZHLN::FileWatchEvent> events;
            {
                ZHLN::FileSystemWatcher watcher;
                const auto handle = watcher.WatchFile(file, [&events](const ZHLN::FileWatchEvent& event) { events.push_back(event); }, 100);
                if (!ZHLN::Test::ExpectTrue(handle != 0)) {
                    return std::unexpected(FileSystemWatcherTestError::ExpectedEventNotObserved);
                }

                // Let the background observer establish the non-existent file
                // baseline, then write twice inside one settle window.
                std::this_thread::sleep_for(150ms);
                {
                    std::ofstream stream {file};
                    stream << "first write";
                }
                std::this_thread::sleep_for(40ms);
                watcher.DispatchEvents();
                ZHLN::Test::ExpectTrue(events.empty());

                {
                    std::ofstream stream {file, std::ios::app};
                    stream << " second write";
                }
                std::this_thread::sleep_for(50ms);
                watcher.DispatchEvents();
                ZHLN::Test::ExpectTrue(events.empty());

                if (!ZHLN::Test::ExpectTrue(DispatchUntil(watcher, events, 1))) {
                    return std::unexpected(FileSystemWatcherTestError::ExpectedEventNotObserved);
                }
                ZHLN::Test::ExpectTrue(events[0].action == ZHLN::FileWatchAction::Created);
                ZHLN::Test::ExpectTrue(events[0].path.lexically_normal() == file.lexically_normal());

                {
                    std::ofstream stream {file, std::ios::app};
                    stream << " settled modification";
                }
                if (!ZHLN::Test::ExpectTrue(DispatchUntil(watcher, events, 2))) {
                    return std::unexpected(FileSystemWatcherTestError::ExpectedEventNotObserved);
                }
                ZHLN::Test::ExpectTrue(events[1].action == ZHLN::FileWatchAction::Modified);

                fs::remove(file, ec);
                if (!ZHLN::Test::ExpectTrue(!ec)) {
                    return std::unexpected(FileSystemWatcherTestError::ExpectedEventNotObserved);
                }
                if (!ZHLN::Test::ExpectTrue(DispatchUntil(watcher, events, 3))) {
                    return std::unexpected(FileSystemWatcherTestError::ExpectedEventNotObserved);
                }
                ZHLN::Test::ExpectTrue(events[2].action == ZHLN::FileWatchAction::Deleted);

                static_cast<void>(watcher.Unwatch(handle));
                {
                    std::ofstream stream {file};
                    stream << "unobserved";
                }
                std::this_thread::sleep_for(150ms);
                watcher.DispatchEvents();
                ZHLN::Test::ExpectTrue(events.size() == size_t {3});
            }

            fs::remove_all(root, ec);
            return {};
        }

        std::expected<void, ZHLN::Error> recursive_directory_filter_routes_only_matching_files() {
            std::error_code ec;
            const fs::path root = UniqueTestDirectory(ec);
            if (!ZHLN::Test::ExpectTrue(!ec)) {
                return std::unexpected(FileSystemWatcherTestError::TemporaryDirectoryFailed);
            }
            const fs::path nested = root / "nested";
            const fs::path shader = nested / "hot_reload.slang";
            fs::create_directories(nested, ec);
            if (!ZHLN::Test::ExpectTrue(!ec)) {
                return std::unexpected(FileSystemWatcherTestError::TemporaryDirectoryFailed);
            }

            std::vector<ZHLN::FileWatchEvent> events;
            {
                ZHLN::FileSystemWatcher watcher;
                const auto handle = watcher.WatchDirectory(
                    root, [&events](const ZHLN::FileWatchEvent& event) { events.push_back(event); }, true, ".slang", 50
                );
                if (!ZHLN::Test::ExpectTrue(handle != 0)) {
                    return std::unexpected(FileSystemWatcherTestError::ExpectedEventNotObserved);
                }

                std::this_thread::sleep_for(150ms);
                {
                    std::ofstream ignored {root / "ignored.txt"};
                    ignored << "not a shader";
                }
                {
                    std::ofstream stream {shader};
                    stream << "shader";
                }

                if (!ZHLN::Test::ExpectTrue(DispatchUntil(watcher, events, 1))) {
                    return std::unexpected(FileSystemWatcherTestError::ExpectedEventNotObserved);
                }
                ZHLN::Test::ExpectTrue(events.size() == size_t {1});
                ZHLN::Test::ExpectTrue(events[0].action == ZHLN::FileWatchAction::Created);
                ZHLN::Test::ExpectTrue(events[0].path.lexically_normal() == shader.lexically_normal());
            }

            fs::remove_all(root, ec);
            return {};
        }
    };
};

} // namespace

auto RunFileSystemWatcherSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<FileSystemWatcherTestSuite>();
}
