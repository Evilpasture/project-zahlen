// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Console/Console.hpp
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

struct ColorRGBA {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

/// Application-owned in-memory console state.
///
/// This is an optional UI/tooling feature, not engine state. Construct one for
/// each host that needs a console and drive it from that host's UI thread; the
/// instance deliberately has no process-global state or global synchronization.
class GameConsole {
  public:
    void Log(std::string_view msg, ColorRGBA color = {});

    [[nodiscard]] bool ConsumeScroll() noexcept;

    void AddHistory(std::string_view cmd);

    [[nodiscard]] auto HistoryPos() noexcept -> int&;

    // Zero-allocation accessors for rendering the owning console instance.
    [[nodiscard]] auto GetEntryCount() const noexcept -> size_t;
    void GetEntry(size_t index, std::string_view& outText, float& outR, float& outG, float& outB, float& outA) const noexcept;

    [[nodiscard]] auto GetHistoryCount() const noexcept -> size_t;
    [[nodiscard]] auto GetHistoryItem(size_t index) const noexcept -> std::string_view;

  private:
    struct Entry {
        std::string text;
        ColorRGBA   color;
    };

    std::vector<std::string> _history;
    int                      _historyPos      = -1;
    std::vector<Entry>       _entries;
    bool                     _scrollToBottom = false;
};

} // namespace ZHLN
