// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FrameDestinations.hpp"
#include <algorithm>
#include <utility>

namespace ZHLN {

FrameDestinations::~FrameDestinations() noexcept                                             = default;
FrameDestinations::FrameDestinations(FrameDestinations&&) noexcept                           = default;
auto FrameDestinations::operator=(FrameDestinations&&) noexcept -> FrameDestinations&         = default;

FrameDestinations::Recording::~Recording() noexcept { Close(); }
FrameDestinations::Recording::Recording(Recording&& other) noexcept: cmd(std::exchange(other.cmd, VK_NULL_HANDLE)), open(std::exchange(other.open, false)) {}
auto FrameDestinations::Recording::operator=(Recording&& other) noexcept -> Recording& {
    if (this != &other) {
        Close();
        cmd  = std::exchange(other.cmd, VK_NULL_HANDLE);
        open = std::exchange(other.open, false);
    }
    return *this;
}

auto FrameDestinations::Recording::Open(VkCommandBuffer slot) noexcept -> VkCommandBuffer {
    if (!open) {
        cmd  = slot;
        open = cmd != VK_NULL_HANDLE;
        if (open) {
            ZHLN_BeginCommandBuffer(cmd);
        }
    }
    return cmd;
}

void FrameDestinations::Recording::Close() noexcept {
    if (open) {
        if (cmd != VK_NULL_HANDLE) {
            ZHLN_EndCommandBuffer(cmd);
        }
        open = false;
    }
}

void FrameDestinations::Recording::Discard() noexcept {
    cmd  = VK_NULL_HANDLE;
    open = false;
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
    it->recording.Discard();
    windows.erase(it);
}

void FrameDestinations::Clear() noexcept {
    for (Window& entry: windows) { entry.recording.Discard(); }
    windows.clear();
    activeWindow = 0;
}

void FrameDestinations::BeginFrame() noexcept {
    activeWindow = 0;
    for (Window& entry: windows) {
        entry.acquired.reset();
        entry.recording.Discard();
    }
}

void FrameDestinations::CloseRecordings() noexcept {
    for (Window& entry: windows) { entry.recording.Close(); }
}

}
