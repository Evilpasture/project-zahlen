// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FrameDestinations.hpp"
#include <algorithm>
#include <utility>

namespace ZHLN {

FrameDestinations::~FrameDestinations() noexcept { Clear(); }
FrameDestinations::FrameDestinations(FrameDestinations&& other) noexcept:
    activeWindow(std::exchange(other.activeWindow, 0)), nextWindowId(std::exchange(other.nextWindowId, 1)), windows(std::move(other.windows)) {}
auto FrameDestinations::operator=(FrameDestinations&& other) noexcept -> FrameDestinations& {
    if (this != &other) {
        Clear();
        activeWindow = std::exchange(other.activeWindow, 0);
        nextWindowId = std::exchange(other.nextWindowId, 1);
        windows = std::move(other.windows);
    }
    return *this;
}

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
    it->recorder.Abort();
    windows.erase(it);
}

void FrameDestinations::Clear() noexcept {
    for (Window& entry: windows) { entry.recorder.Abort(); }
    windows.clear();
    activeWindow = 0;
}

void FrameDestinations::BeginFrame() noexcept {
    activeWindow = 0;
    for (Window& entry: windows) {
        entry.recorder.Abort();
        entry.acquired.reset();
    }
}

void FrameDestinations::AbortRecordings() noexcept {
    for (Window& entry: windows) { entry.recorder.Abort(); }
}

}
