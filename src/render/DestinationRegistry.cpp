// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/DestinationRegistry.cpp

#include "DestinationRegistry.hpp"
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <utility>

namespace ZHLN {

DestinationRegistry::~DestinationRegistry() noexcept                                         = default;
DestinationRegistry::DestinationRegistry(DestinationRegistry&&) noexcept                     = default;
auto DestinationRegistry::operator=(DestinationRegistry&&) noexcept -> DestinationRegistry& = default;

// ============================================================================
// Window table
// ============================================================================

auto DestinationRegistry::Find(const Window& window) noexcept -> WindowEntry* {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const WindowEntry& entry) { return entry.window == &window; });
    return it != windows.end() ? &*it : nullptr;
}

auto DestinationRegistry::Windows() noexcept -> std::span<WindowEntry> {
    return windows;
}

auto DestinationRegistry::Full() const noexcept -> bool {
    return windows.size() >= kMaxWindows;
}

void DestinationRegistry::Attach(WindowEntry entry) noexcept {
    windows.push_back(std::move(entry));
    // Say which session the new destination uses. A window that is not the
    // renderer's primary one owns its session, and a frame that renders into it
    // is not the frame the primary session presents -- which is worth a line,
    // because from the outside that is a black window with no other symptom.
    ZHLN::Log(
        "[Render] Destination created for window {:p} (primary={}); {}", static_cast<const void*>(windows.back().window),
        windows.back().IsPrimary() ? 1 : 0, windows.back().IsPrimary() ? "borrowing the renderer's session" : "owning its own session"
    );
}

void DestinationRegistry::Detach(const Window& window) noexcept {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const WindowEntry& entry) { return entry.window == &window; });
    if (it == windows.end()) {
        return;
    }
    if (activeWindow == it->window) {
        activeWindow = nullptr;
    }
    windows.erase(it);
}

void DestinationRegistry::Clear() noexcept {
    windows.clear();
    records.clear();
    activeWindow = nullptr;
}

auto DestinationRegistry::LiveGeneration(const Window& window) noexcept -> uint64_t {
    if (auto* entry = Find(window); entry != nullptr) {
        return entry->Session().presentation.resourceGeneration;
    }
    return 0;
}

// ============================================================================
// Records
// ============================================================================

auto DestinationRegistry::Register(Record record) noexcept -> Handle {
    // A retired slot (no handle, no view) is free again. Reusing it instead of
    // appending keeps the registry bounded by live records rather than by every
    // registration ever made; the handle carries the index explicitly, so a
    // recycled slot is still addressable by the callers that hold its handle.
    size_t index = records.size();
    for (size_t i = 0; i < records.size(); ++i) {
        const Record& slot = records[i];
        if (!slot.handle.Valid() && slot.view == VK_NULL_HANDLE && slot.image == VK_NULL_HANDLE) {
            index = i;
            break;
        }
    }

    // The serial is minted per registration, so a handle vended before this
    // record existed cannot resolve to it even though the slot index is reused.
    // 0 is the retired marker, so the counter steps over it.
    uint32_t serial = Handle::WrapSerial(nextSerial++);
    if (serial == 0) {
        serial = Handle::WrapSerial(nextSerial++);
    }
    record.serial = serial;
    record.handle = Handle::Make(static_cast<uint32_t>(index), serial);

    if (index == records.size()) {
        records.push_back(record);
    } else {
        records[index] = record;
    }
    return record.handle;
}

auto DestinationRegistry::Resolve(const RenderAttachment& attachment) noexcept -> std::optional<Record> {
    if (!attachment.Valid()) {
        return std::nullopt;
    }
    const auto handle = Handle::FromTexture(attachment.texture);
    if (!handle.has_value() || handle->Index() >= records.size()) {
        return std::nullopt;
    }
    const Record& record = records[handle->Index()];
    // Slot identity, not just slot number: a record retired since this handle
    // was vended has serial 0, and a different image living in the same slot
    // has a different one.
    if (record.serial != handle->Serial()) {
        return std::nullopt;
    }
    // A window-backed record is only valid while the presentation resources it
    // was built from are still the live ones. Resolving after a rebuild would
    // bind a destroyed VkImage/VkImageView, which is a use-after-free the
    // driver reports as an invalid handle at best and segfaults on at worst --
    // so refuse, loudly, and let the caller draw nothing this frame.
    if (record.window != nullptr && record.generation != LiveGeneration(*record.window)) {
        ZHLN::Log("[Render] Attachment from a retired presentation generation; pass skipped.");
        return std::nullopt;
    }
    return record;
}

auto DestinationRegistry::Records() noexcept -> std::span<Record> {
    return records;
}

void DestinationRegistry::NoteWritten(const RenderAttachment& attachment, Vk::AttachmentLayout layout) noexcept {
    if (!attachment.Valid()) {
        return;
    }
    const auto handle = Handle::FromTexture(attachment.texture);
    if (!handle.has_value() || handle->Index() >= records.size()) {
        return;
    }
    Record& record = records[handle->Index()];
    if (record.serial != handle->Serial()) {
        return;
    }
    record.writtenThisFrame = true;
    record.backgroundFilled = false;
    record.trackedLayout    = layout;
    // A frame that writes its destination again re-arms the unwritten warning,
    // so the next episode is reported too.
    unwrittenWarned = false;
}

void DestinationRegistry::Retire(const Window* owner) noexcept {
    if (owner == nullptr) {
        // A null owner is the render-to-texture family (not owned by a window);
        // retiring "everything without a window" is never what a caller means.
        return;
    }
    for (Record& record: records) {
        if (record.window != owner) {
            continue;
        }
        // Neutralize in place: the slot index stays allocated so no other
        // destination's recordSlots entry shifts, but every handle and image it
        // named is gone. Resolve rejects the mismatch.
        record.handle           = {};
        record.serial           = 0;
        record.image            = VK_NULL_HANDLE;
        record.view             = VK_NULL_HANDLE;
        record.bindlessIndex    = 0;
        record.generation       = 0;
        record.trackedLayout    = Vk::AttachmentLayout::Undefined;
        record.writtenThisFrame = false;
        record.backgroundFilled = false;
    }
}

// ============================================================================
// The frame's active destination
// ============================================================================

void DestinationRegistry::BeginFrame() noexcept {
    activeWindow = nullptr;
    // Every window starts the frame un-acquired. The image it was presenting is
    // still being read by the fence this frame waited on, and the command
    // buffer that was recording into it was closed and submitted at the end of
    // the last one; both are re-established by the next vend.
    for (WindowEntry& entry: windows) {
        entry.imageAcquired = false;
        entry.openCmd       = VK_NULL_HANDLE;
        entry.commandOpen   = false;
    }
}

void DestinationRegistry::SetActive(Window* window) noexcept {
    activeWindow = window;
}

auto DestinationRegistry::ActiveWindow() const noexcept -> Window* {
    return activeWindow;
}

auto DestinationRegistry::ActiveRecord() noexcept -> std::optional<Record> {
    if (activeWindow == nullptr) {
        return std::nullopt;
    }
    WindowEntry* entry = Find(*activeWindow);
    if (entry == nullptr || !entry->imageAcquired || entry->recordSlots.empty()) {
        return std::nullopt;
    }
    const uint32_t slot = entry->recordSlots[entry->imageIndex];
    if (slot == 0 || slot - 1 >= records.size()) {
        return std::nullopt;
    }
    const Record& record = records[slot - 1];
    // A retired slot keeps its index but loses its image, view and serial.
    if (record.serial == 0 || record.image == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    return record;
}

// ============================================================================
// Unwritten-destination warning
// ============================================================================

auto DestinationRegistry::UnwrittenWarned() const noexcept -> bool {
    return unwrittenWarned;
}

void DestinationRegistry::NoteUnwrittenWarned() noexcept {
    unwrittenWarned = true;
}

} // namespace ZHLN
