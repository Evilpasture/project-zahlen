// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "DestinationRegistry.hpp"
#include <algorithm>
#include <utility>

namespace ZHLN {

DestinationRegistry::~DestinationRegistry() noexcept                                         = default;
DestinationRegistry::DestinationRegistry(DestinationRegistry&&) noexcept                     = default;
auto DestinationRegistry::operator=(DestinationRegistry&&) noexcept -> DestinationRegistry& = default;


auto DestinationRegistry::Find(const PresentationTarget& target) noexcept -> WindowEntry* {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const WindowEntry& entry) { return entry.target == &target; });
    return it != windows.end() ? &*it : nullptr;
}

auto DestinationRegistry::Find(const PresentationTarget& target) const noexcept -> const WindowEntry* {
    return const_cast<DestinationRegistry*>(this)->Find(target);
}

auto DestinationRegistry::Windows() noexcept -> std::span<WindowEntry> {
    return windows;
}

auto DestinationRegistry::Full() const noexcept -> bool {
    return windows.size() >= kMaxWindows;
}

auto DestinationRegistry::Attach(WindowEntry entry) noexcept -> WindowEntry* {
    if (Full()) {
        return nullptr;
    }
    windows.push_back(std::move(entry));
    return &windows.back();
}

void DestinationRegistry::Detach(const PresentationTarget& target) noexcept {
    const auto it = std::find_if(windows.begin(), windows.end(), [&](const WindowEntry& entry) { return entry.target == &target; });
    if (it == windows.end()) {
        return;
    }
    if (activeTarget == it->target) {
        activeTarget = nullptr;
    }
    it->recording.Discard();
    windows.erase(it);
}

void DestinationRegistry::Clear() noexcept {
    for (WindowEntry& entry: windows) {
        entry.recording.Discard();
    }
    windows.clear();
    records.clear();
    activeTarget = nullptr;
}

auto DestinationRegistry::LiveGeneration(const PresentationTarget& target) noexcept -> uint64_t {
    if (auto* entry = Find(target); entry != nullptr) {
        return entry->Presenter().resourceGeneration;
    }
    return 0;
}


auto DestinationRegistry::Register(Record record) noexcept -> Handle {
    size_t index = records.size();
    for (size_t i = 0; i < records.size(); ++i) {
        const Record& slot = records[i];
        if (!slot.handle.Valid() && !slot.image.Valid()) {
            index = i;
            break;
        }
    }

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
    if (record.serial != handle->Serial()) {
        if (record.serial == 0 || !record.image.Valid()) {
            return std::unexpected(Miss {.reason = Miss::Reason::SlotRetired, .asked = *handle});
        }
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

    if (record.target != nullptr && record.generation != LiveGeneration(*record.target)) {
        return std::unexpected(Miss {.reason = Miss::Reason::StaleGeneration, .asked = *handle, .target = record.target});
    }
    return record;
}

auto DestinationRegistry::Records() noexcept -> std::span<Record> {
    return records;
}

void DestinationRegistry::NoteWritten(const RenderAttachment& attachment, Rendered::By by, Vk::AttachmentLayout layout) noexcept {
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
    record.content       = Rendered {.by = by};
    record.trackedLayout = layout;
    unwrittenWarned = false;
}

void DestinationRegistry::Retire(const PresentationTarget* owner) noexcept {
    if (owner == nullptr) {
        return;
    }
    for (Record& record: records) {
        if (record.target != owner) {
            continue;
        }
        record.handle           = {};
        record.serial           = 0;
        record.image            = {};
        record.bindlessIndex    = 0;
        record.generation       = 0;
        record.trackedLayout    = Vk::AttachmentLayout::Undefined;
        record.content.reset();
    }
}


auto DestinationRegistry::Record::GetRenderedContent() const noexcept -> FrameOutcome<Rendered> {
    if (!image.Valid()) {
        return std::unexpected(DestinationError::SlotRetired);
    }
    if (!content.has_value()) {
        return std::nullopt;
    }
    return *content;
}


void DestinationRegistry::BeginFrame() noexcept {
    activeTarget = nullptr;
    for (WindowEntry& entry: windows) {
        entry.imageAcquired = false;
        entry.recording.Discard();
    }
}

void DestinationRegistry::SetActive(const PresentationTarget* target) noexcept {
    activeTarget = target;
}

auto DestinationRegistry::ActiveTarget() const noexcept -> const PresentationTarget* {
    return activeTarget;
}

auto DestinationRegistry::ActiveDestination() const noexcept -> const WindowEntry* {
    return activeTarget != nullptr ? Find(*activeTarget) : nullptr;
}

auto DestinationRegistry::ActiveImageIndex() const noexcept -> uint32_t {
    const WindowEntry* active = ActiveDestination();
    return active != nullptr ? active->imageIndex : 0;
}

auto DestinationRegistry::DestinationOf(const Record& record) const noexcept -> const WindowEntry* {
    if (record.target != nullptr) {
        return Find(*record.target);
    }
    return ActiveDestination();
}

void DestinationRegistry::CloseRecordings() noexcept {
    for (WindowEntry& entry: windows) {
        entry.recording.Close();
    }
}

auto DestinationRegistry::ActiveRecord() noexcept -> std::expected<Record, Miss> {
    if (activeTarget == nullptr) {
        return std::unexpected(Miss {.reason = Miss::Reason::NothingVended});
    }
    WindowEntry* entry = Find(*activeTarget);
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
    if (record.serial == 0 || !record.image.Valid()) {
        return std::unexpected(Miss {.reason = Miss::Reason::SlotRetired});
    }
    return record;
}


auto DestinationRegistry::UnwrittenWarned() const noexcept -> bool {
    return unwrittenWarned;
}

void DestinationRegistry::NoteUnwrittenWarned() noexcept {
    unwrittenWarned = true;
}

}
