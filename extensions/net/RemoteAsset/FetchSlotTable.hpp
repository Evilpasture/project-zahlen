// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <RemoteAsset/FetchJob.hpp>
#include <Zahlen/Threading/Mutex.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace ZHLN::Remote {

struct FetchHandle {
    uint32_t slotIndex  = 0;
    uint32_t generation = 0;

    [[nodiscard]] constexpr auto IsValid() const noexcept -> bool {
        return generation != 0;
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return IsValid();
    }

    constexpr auto operator==(const FetchHandle&) const noexcept -> bool = default;
};

class FetchSlotTable {
  public:
    static constexpr size_t kMaxSlots = 128;

    struct Slot {
        ZHLN::Atomic<uint32_t> generation {1};
        ZHLN::Atomic<bool>     occupied {false};
        FetchJob*              job {nullptr};
    };

    FetchSlotTable() = default;

    auto Allocate(FetchJob* job) -> std::optional<FetchHandle> {
        if (job == nullptr) {
            return std::nullopt;
        }
        ZHLN::MutexGuard lock(m_mutex);
        for (uint32_t i = 0; i < kMaxSlots; ++i) {
            Slot& slot     = m_slots[i];
            bool  expected = false;
            if (!slot.occupied.compare_exchange_strong(expected, true, std::memory_order::acquire)) {
                continue;
            }
            job->Retain();
            slot.job               = job;
            const uint32_t generation = slot.generation.load(std::memory_order::relaxed);
            return FetchHandle {.slotIndex = i, .generation = generation};
        }
        return std::nullopt;
    }

    // Returns the job with an extra Retain. Caller must Release. Null if the
    // handle is stale or the slot is empty.
    [[nodiscard]] auto GetJobAndRetain(FetchHandle handle) const -> FetchJob* {
        if (handle.slotIndex >= kMaxSlots || handle.generation == 0) {
            return nullptr;
        }
        ZHLN::MutexGuard lock(m_mutex);
        const Slot&      slot = m_slots[handle.slotIndex];
        if (slot.generation.load(std::memory_order::acquire) != handle.generation ||
            !slot.occupied.load(std::memory_order::relaxed) || slot.job == nullptr) {
            return nullptr;
        }
        slot.job->Retain();
        return slot.job;
    }

    void Free(FetchHandle handle) {
        if (handle.slotIndex >= kMaxSlots) {
            return;
        }
        FetchJob* jobToRelease = nullptr;
        {
            ZHLN::MutexGuard lock(m_mutex);
            Slot&            slot = m_slots[handle.slotIndex];
            if (slot.generation.load(std::memory_order::relaxed) != handle.generation) {
                return;
            }
            jobToRelease = slot.job;
            slot.job     = nullptr;
            slot.generation.fetch_add(1, std::memory_order::relaxed);
            if (slot.generation.load(std::memory_order::relaxed) == 0) {
                slot.generation.store(1, std::memory_order::relaxed);
            }
            slot.occupied.store(false, std::memory_order::release);
        }
        if (jobToRelease != nullptr) {
            jobToRelease->Release();
        }
    }

    template <typename Fn>
    void ForEachOccupied(Fn&& fn) {
        ZHLN::MutexGuard lock(m_mutex);
        for (uint32_t i = 0; i < kMaxSlots; ++i) {
            Slot& slot = m_slots[i];
            if (!slot.occupied.load(std::memory_order::relaxed) || slot.job == nullptr) {
                continue;
            }
            fn(FetchHandle {.slotIndex = i, .generation = slot.generation.load(std::memory_order::relaxed)}, slot.job);
        }
    }

  private:
    mutable ZHLN::Mutex         m_mutex;
    std::array<Slot, kMaxSlots> m_slots {};
};

} // namespace ZHLN::Remote
