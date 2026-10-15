// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ===========================================================================
// The last N samples, in order.
//
// Nearly everything that draws a history -- a frame-time chart, a sparkline, a
// per-core usage bar strip, a rolling average -- wants the same thing: push a
// sample per frame, and ask for the newest ones. Written inside a panel that is
// four lines of index arithmetic, and it is exactly those four lines that went
// wrong once: the chart indexed its ring from the *oldest* sample, so once the
// window filled it re-projected the same 128 samples for the rest of the run --
// a frozen chart, drawn correctly, in the panel's own copy of the pattern.
//
// So the pattern is here instead, where the invariant is written down:
//
//   * the window holds the newest Count() samples, in the order they arrived;
//   * At(0) is the newest and At(Count() - 1) is the oldest, whatever the ring's
//     write cursor happens to be;
//   * CopyNewest writes up to `out.size()` samples into the *end* of `out`, so
//     out.back() is the sample that just arrived: the newest sample of a strip is
//     always its last element, whether the strip is full or still filling;
//   * CopyOldest writes the window's first samples, oldest first, for the callers
//     that want the whole thing (a percentile partition, a total).
//
// A window's samples are only contiguous when the ring has not wrapped, so the
// two projections copy into caller storage rather than handing out a span of
// their own -- and the caller says how much room it has, which is what makes
// "the newest 128 of a 10 000 sample window" a request the widget can answer.
//
// Capacity is fixed: a chart knows how many bars it has, and a fixed window
// allocates once, at construction, and never again.
// ===========================================================================

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace ZHLN::GUI::Widget {

// One rolling window of `Capacity` samples of `T`.
template <typename T, size_t Capacity>
class RollingWindow {
  public:
    static_assert(Capacity > 0, "A rolling window needs room for at least one sample.");

    static constexpr size_t kCapacity = Capacity;

    // Adds a sample. A full window drops its oldest to make room.
    void Push(T value) noexcept {
        _values[_write] = std::move(value);
        _write          = (_write + 1) % Capacity;
        if (_count < Capacity) {
            ++_count;
        }
    }

    void Clear() noexcept {
        _write = 0;
        _count = 0;
    }

    // Samples currently held: Capacity once the window has filled.
    [[nodiscard]] auto Count() const noexcept -> size_t {
        return _count;
    }

    [[nodiscard]] auto Empty() const noexcept -> bool {
        return _count == 0;
    }

    [[nodiscard]] auto Full() const noexcept -> bool {
        return _count == Capacity;
    }

    // The sample `age` samples back, 0 being the newest. `age` must be less than
    // Count(); the assertion is what keeps a caller from reading a slot the
    // window never wrote.
    [[nodiscard]] auto At(size_t age) const noexcept -> const T& {
        // Back from the slot just written -- which is the newest sample whatever
        // _write is -- not forward from the oldest. Indexed the other way round
        // this answers with the window's first samples once it fills, and a
        // caller asking for the newest N gets the same N forever.
        return _values[(_write + Capacity - 1 - age) % Capacity];
    }

    [[nodiscard]] auto Newest() const noexcept -> const T& {
        return At(0);
    }

    [[nodiscard]] auto Oldest() const noexcept -> const T& {
        return At(_count - 1);
    }

    // The newest up to `out.size()` samples, written into the end of `out` so
    // that out.back() is the newest. Slots before them are left as they were --
    // a caller that wants them blank (a chart that has not filled) blanks them
    // itself, and a caller that does not care is not paying for a fill.
    // Returns how many samples were written.
    auto CopyNewest(std::span<T> out) const noexcept -> size_t {
        const size_t count = (out.size() < _count) ? out.size() : _count;
        const size_t first = out.size() - count;
        for (size_t i = 0; i < count; ++i) {
            out[first + i] = At(count - 1 - i);
        }
        return count;
    }

    // The window's oldest up to `out.size()` samples, oldest first. A window
    // longer than `out` copies its first samples rather than its last, so a
    // caller that sized `out` for its own reason always sees a full window from
    // the start of it.
    auto CopyOldest(std::span<T> out) const noexcept -> size_t {
        const size_t count = (out.size() < _count) ? out.size() : _count;
        for (size_t i = 0; i < count; ++i) {
            out[i] = At(_count - 1 - i);
        }
        return count;
    }

    // Every held sample in the order it arrived, oldest first.
    template <typename Fn>
    void ForEachOldest(Fn&& fn) const {
        for (size_t i = 0; i < _count; ++i) {
            fn(At(_count - 1 - i));
        }
    }

  private:
    std::array<T, Capacity> _values {};
    size_t                  _write = 0; // the slot the next sample goes into
    size_t                  _count = 0; // how many slots are live
};

} // namespace ZHLN::GUI::Widget
