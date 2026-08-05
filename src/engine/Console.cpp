// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Console.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <string>
#include <vector>

std::vector<std::string> s_InvShellLog;
bool                     s_InvScrollToBottom = false;

namespace ZHLN {

namespace {

struct ConsoleEntryInternal {
    std::string text;
    ColorRGBA   color;
};

static std::vector<std::string>          s_History;
static int                               s_HistoryPos = -1;
static std::vector<ConsoleEntryInternal> s_Entries;
static ZHLN::Mutex                       s_Mutex;
static bool                              s_ScrollToBottom = false;

} // namespace

void GameConsole::Log(std::string_view msg, ColorRGBA color) {
    ZHLN_LOCK(s_Mutex) {
        s_Entries.push_back({.text = std::string(msg), .color = color});
        s_ScrollToBottom = true;
    }
}

bool GameConsole::ConsumeScroll() {
    ZHLN_LOCK(s_Mutex) {
        bool s           = s_ScrollToBottom;
        s_ScrollToBottom = false;
        return s;
    }
}

void GameConsole::AddHistory(std::string_view cmd) {
    ZHLN_LOCK(s_Mutex) {
        if (s_History.empty() || s_History.back() != cmd) {
            s_History.emplace_back(cmd);
        }
        s_HistoryPos = -1;
    }
}

int& GameConsole::HistoryPos() {
    return s_HistoryPos;
}

size_t GameConsole::GetEntryCount() noexcept {
    ZHLN_LOCK(s_Mutex) {
        return s_Entries.size();
    }
}

void GameConsole::GetEntry(size_t index, std::string_view& outText, float& outR, float& outG, float& outB, float& outA) noexcept {
    ZHLN_LOCK(s_Mutex) {
        if (index < s_Entries.size()) {
            outText = s_Entries[index].text;
            outR    = s_Entries[index].color.r;
            outG    = s_Entries[index].color.g;
            outB    = s_Entries[index].color.b;
            outA    = s_Entries[index].color.a;
        }
    }
}

size_t GameConsole::GetHistoryCount() noexcept {
    ZHLN_LOCK(s_Mutex) {
        return s_History.size();
    }
}

std::string_view GameConsole::GetHistoryItem(size_t index) noexcept {
    static thread_local std::string t_tempItem;
    ZHLN_LOCK(s_Mutex) {
        if (index < s_History.size()) {
            t_tempItem = s_History[index];
            return t_tempItem;
        }
        return "";
    }
}

} // namespace ZHLN
