// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/DestinationRegistry.cpp

#include "DestinationRegistry.hpp"
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

auto DestinationRegistry::Find(const Window& window) const noexcept -> const WindowEntry* {
    return const_cast<DestinationRegistry*>(this)->Find(window);
}

auto DestinationRegistry::Windows() noexcept -> std::span<WindowEntry> {
    return windows;
}

auto DestinationRegistry::Full() const noexcept -> bool {
    return windows.size() >= kMaxWindows;
}

auto DestinationRegistry::Attach(WindowEntry entry) noexcept -> WindowEntry* {
    if (Full()) {
        // The caller checks Full() before it builds a surface and a presenter --
        // that is the caller's error to report, with its own vocabulary. This
        // is the backstop that keeps the table at kMaxWindows either way.
        return nullptr;
    }
    windows.push_back(std::move(entry));
    return &windows.back();
}

void DestinationRegistry::Detach(const Window& window) noexcept {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const WindowEntry& entry) { return entry.window == &window; });
    if (it == windows.end()) {
        return;
    }
    if (activeWindow == it->window) {
        activeWindow = nullptr;
    }
    // The presenter this recording's buffer came from is going away with the
    // entry, so the buffer is forgotten, not ended.
    it->recording.Discard();
    windows.erase(it);
}

void DestinationRegistry::Clear() noexcept {
    for (WindowEntry& entry: windows) {
        entry.recording.Discard();
    }
    windows.clear();
    records.clear();
    activeWindow = nullptr;
}

auto DestinationRegistry::LiveGeneration(const Window& window) noexcept -> uint64_t {
    if (auto* entry = Find(window); entry != nullptr) {
        return entry->Presenter().resourceGeneration;
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

auto DestinationRegistry::Resolve(const RenderAttachment& attachment) noexcept -> std::expected<Record, Miss> {
    if (!attachment.Valid()) {
        return std::unexpected(Miss {.reason = Miss::Reason::NotAHandle});
    }
    const auto handle = Handle::FromTexture(attachment.texture);
    if (!handle.has_value()) {
        return std::unexpected(Miss {.reason = Miss::Reason::NotAHandle});
    }
    if (handle->Index() >= records.size()) {
        return std::unexpected(Miss {.reason = Miss::Reason::SlotNeverHeld, .asked = *handle});
    }

    const Record& record = records[handle->Index()];
    // Slot identity, not just slot number: a record retired since this handle
    // was vended has serial 0, and a different image living in the same slot
    // has a different one.
    if (record.serial != handle->Serial()) {
        // Two ways for the slot to be someone else's, and they are not the same
        // news. Retired: the destination is gone and nothing took its place.
        if (record.serial == 0 || record.image == VK_NULL_HANDLE) {
            return std::unexpected(Miss {.reason = Miss::Reason::SlotRetired, .asked = *handle});
        }
        // Re-vended: a live record holds the slot. Whether that record is the
        // *frame's* destination is the one distinction a caller cannot make for
        // itself without asking around -- so it is answered here, because this
        // is the object that knows which window the frame is rendering into.
        const bool thisFrames = [&] {
            const auto active = ActiveRecord();
            return active.has_value() && active->handle.Index() == handle->Index();
        }();
        return std::unexpected(Miss {
            .reason = thisFrames ? Miss::Reason::SlotReVendedThisFrame : Miss::Reason::SlotReVended,
            .asked  = *handle,
            .live   = record,
        });
    }

    // A window-backed record is only valid while the presentation resources it
    // was built from are still the live ones. Resolving after a rebuild would
    // bind a destroyed VkImage/VkImageView, which is a use-after-free the
    // driver reports as an invalid handle at best and segfaults on at worst --
    // so refuse, and let the caller draw nothing this frame. The window is
    // named in the miss: it is the one whose rebuild invalidated the record.
    if (record.window != nullptr && record.generation != LiveGeneration(*record.window)) {
        return std::unexpected(Miss {.reason = Miss::Reason::StaleGeneration, .asked = *handle, .window = record.window});
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
        // destination's recordHandles entry shifts, but every handle and image
        // it named is gone. Resolve rejects the mismatch.
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
    // still being read by the fence this frame waited on, and the recording that
    // was writing into it was ended at the end of the last frame (or by the
    // frame's own guard); both are re-established by the next acquisition.
    for (WindowEntry& entry: windows) {
        entry.imageAcquired = false;
        // A recording that is somehow still open belongs to the frame that just
        // ended, and the pool it names is reset before the next acquire -- so
        // the handle is dropped rather than ended. Ending a buffer from a pool
        // that is about to be reset would be the only wrong move here.
        entry.recording.Discard();
    }
}

void DestinationRegistry::SetActive(Window* window) noexcept {
    activeWindow = window;
}

auto DestinationRegistry::ActiveWindow() const noexcept -> Window* {
    return activeWindow;
}

auto DestinationRegistry::ActiveDestination() const noexcept -> const WindowEntry* {
    return activeWindow != nullptr ? Find(*activeWindow) : nullptr;
}

auto DestinationRegistry::ActiveImageIndex() const noexcept -> uint32_t {
    const WindowEntry* active = ActiveDestination();
    return active != nullptr ? active->imageIndex : 0;
}

auto DestinationRegistry::DestinationOf(const Record& record) const noexcept -> const WindowEntry* {
    if (record.window != nullptr) {
        return Find(*record.window);
    }
    // A record with no window is a render texture. It has no submission of its
    // own -- nothing presents it -- so its commands ride the frame's stream,
    // which is the destination the frame is drawing into.
    return ActiveDestination();
}

void DestinationRegistry::CloseRecordings() noexcept {
    for (WindowEntry& entry: windows) {
        entry.recording.Close();
    }
}

auto DestinationRegistry::ActiveRecord() noexcept -> std::expected<Record, Miss> {
    if (activeWindow == nullptr) {
        // Nothing was vended this frame, so there is no destination to have
        // missed: the frame has not asked for one yet.
        return std::unexpected(Miss {.reason = Miss::Reason::NothingVended});
    }
    WindowEntry* entry = Find(*activeWindow);
    if (entry == nullptr || !entry->imageAcquired || entry->imageIndex >= entry->recordHandles.size()) {
        return std::unexpected(Miss {.reason = Miss::Reason::NothingVended});
    }
    const Handle handle = entry->recordHandles[entry->imageIndex];
    if (!handle.Valid()) {
        return std::unexpected(Miss {.reason = Miss::Reason::NothingVended});
    }
    if (handle.Index() >= records.size()) {
        return std::unexpected(Miss {.reason = Miss::Reason::SlotNeverHeld});
    }
    const Record& record = records[handle.Index()];
    // A retired slot keeps its index but loses its image, view and serial.
    if (record.serial == 0 || record.image == VK_NULL_HANDLE) {
        return std::unexpected(Miss {.reason = Miss::Reason::SlotRetired});
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
