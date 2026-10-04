// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Console/Console.hpp>

namespace ZHLN {

void GameConsole::Log(std::string_view msg, ColorRGBA color) {
    _entries.push_back({.text = std::string(msg), .color = color});
    _scrollToBottom = true;
}

bool GameConsole::ConsumeScroll() noexcept {
    const bool scrollToBottom = _scrollToBottom;
    _scrollToBottom           = false;
    return scrollToBottom;
}

void GameConsole::AddHistory(std::string_view cmd) {
    if (_history.empty() || _history.back() != cmd) {
        _history.emplace_back(cmd);
    }
    _historyPos = -1;
}

auto GameConsole::HistoryPos() noexcept -> int& {
    return _historyPos;
}

auto GameConsole::GetEntryCount() const noexcept -> size_t {
    return _entries.size();
}

void GameConsole::GetEntry(size_t index, std::string_view& outText, float& outR, float& outG, float& outB, float& outA) const noexcept {
    if (index < _entries.size()) {
        const Entry& entry = _entries[index];
        outText            = entry.text;
        outR               = entry.color.r;
        outG               = entry.color.g;
        outB               = entry.color.b;
        outA               = entry.color.a;
    }
}

auto GameConsole::GetHistoryCount() const noexcept -> size_t {
    return _history.size();
}

auto GameConsole::GetHistoryItem(size_t index) const noexcept -> std::string_view {
    return index < _history.size() ? std::string_view {_history[index]} : std::string_view {};
}

} // namespace ZHLN
