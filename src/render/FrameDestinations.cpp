// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FrameDestinations.hpp"
#include <algorithm>
#include <utility>

namespace ZHLN {

FrameDestinations::~FrameDestinations() noexcept { Clear(); }

auto FrameDestinations::Find(const PresentationTarget& target) noexcept -> Window* {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const Window& entry) { return entry.target == &target; });
    return it != windows.end() ? &*it : nullptr;
}

auto FrameDestinations::Find(const PresentationTarget& target) const noexcept -> const Window* {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const Window& entry) { return entry.target == &target; });
    return it != windows.end() ? &*it : nullptr;
}

auto FrameDestinations::Find(uint64_t id) noexcept -> Window* {
    if (id == 0) { return nullptr; }
    const auto it = std::find_if(windows.begin(), windows.end(), [id](const Window& entry) { return entry.id == id; });
    return it != windows.end() ? &*it : nullptr;
}

auto FrameDestinations::Find(uint64_t id) const noexcept -> const Window* {
    if (id == 0) { return nullptr; }
    const auto it = std::find_if(windows.begin(), windows.end(), [id](const Window& entry) { return entry.id == id; });
    return it != windows.end() ? &*it : nullptr;
}

auto FrameDestinations::Windows() noexcept -> std::span<Window> { return windows; }

auto FrameDestinations::FindRecording(uint64_t windowId) noexcept -> Recording* {
    const auto it = std::find_if(recordings.begin(), recordings.end(), [windowId](const Recording& entry) { return entry.windowId == windowId; });
    return it != recordings.end() ? &*it : nullptr;
}

auto FrameDestinations::FindRecording(uint64_t windowId) const noexcept -> const Recording* {
    const auto it = std::find_if(recordings.begin(), recordings.end(), [windowId](const Recording& entry) { return entry.windowId == windowId; });
    return it != recordings.end() ? &*it : nullptr;
}

void FrameDestinations::AddRecording(uint64_t windowId, Vk::CommandRecorder&& recorder) noexcept {
    recordings.emplace_back(windowId, std::move(recorder));
}

void FrameDestinations::AbortRecording(uint64_t windowId) noexcept {
    const auto it = std::find_if(recordings.begin(), recordings.end(), [windowId](const Recording& entry) { return entry.windowId == windowId; });
    if (it != recordings.end()) {
        std::move(it->recorder).Abort();
        recordings.erase(it);
    }
}

auto FrameDestinations::Full() const noexcept -> bool { return windows.size() >= kMaxWindows; }

auto FrameDestinations::Attach(Window entry) noexcept -> Window* {
    if (Full()) { return nullptr; }
    entry.id = nextWindowId++;
    windows.push_back(std::move(entry));
    return &windows.back();
}

void FrameDestinations::Detach(const PresentationTarget& target) noexcept {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const Window& entry) { return entry.target == &target; });
    if (it == windows.end()) { return; }
    if (activeWindow == it->id) { activeWindow = 0; }
    AbortRecording(it->id);
    windows.erase(it);
}

void FrameDestinations::Clear() noexcept {
    AbortRecordings();
    windows.clear();
    activeWindow = 0;
}

void FrameDestinations::BeginFrame() noexcept {
    activeWindow = 0;
    AbortRecordings();
    for (Window& entry: windows) {
        entry.acquired.reset();
    }
}

void FrameDestinations::AbortRecordings() noexcept {
    for (Recording& entry: recordings) {
        std::move(entry.recorder).Abort();
    }
    recordings.clear();
}

}
