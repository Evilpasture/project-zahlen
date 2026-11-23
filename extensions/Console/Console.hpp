// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <optional>
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

// Application-owned in-memory console state.
//
// This is an optional UI/tooling feature, not engine state. Construct one for
// each host that needs a console and drive it from that host's UI thread; the
// instance deliberately has no process-global state or global synchronization.
struct ConsoleEntry {
    std::string_view text;
    ColorRGBA        color {1.0f, 1.0f, 1.0f, 1.0f};
};

class GameConsole {
  public:
    void Log(std::string_view msg, ColorRGBA color = {});

    [[nodiscard]] bool ConsumeScroll() noexcept;

    void AddHistory(std::string_view cmd);

    [[nodiscard]] auto HistoryPos() noexcept -> int&;

    // Zero-allocation accessors for rendering the owning console instance.
    [[nodiscard]] auto GetEntryCount() const noexcept -> size_t;
    [[nodiscard]] auto GetEntry(size_t index) const noexcept -> std::optional<ConsoleEntry>;

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
