// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ztest's interactive crash replay interface. The terminal and process-launch
// implementations are private to DebuggerReplay.cpp, not included by clients.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ztest::Debugger {

struct Replay {
    std::vector<std::string>          argv; // worker binary, then its case-selection flags
    std::string                       workingDir;
    std::map<std::string, std::string> environment;
};

enum class Kind { Gdb, Lldb };

struct Tool {
    Kind        kind;
    std::string path;
};

enum class PromptChoice { Launch, SkipOnce, SkipRemaining };

// One policy instance per ztest run. Only an explicit "skip remaining" choice
// turns off future prompts; an ordinary N leaves the next crash askable.
class PromptPolicy {
  public:
    [[nodiscard]] auto ShouldOffer() const noexcept -> bool;
    [[nodiscard]] auto Accept(PromptChoice choice) noexcept -> bool; // true only for Launch

  private:
    bool m_skipRemaining = false;
};

[[nodiscard]] auto FindTool(const Replay& replay) -> std::optional<Tool>;
[[nodiscard]] auto BuildDebuggerArgv(const Tool& tool, const Replay& replay) -> std::vector<std::string>;
[[nodiscard]] auto HasInteractiveTerminal() noexcept -> bool;
[[nodiscard]] auto IsYes(std::string_view answer) noexcept -> bool;
[[nodiscard]] auto IsNo(std::string_view answer) noexcept -> bool;
[[nodiscard]] auto IsSkipRemaining(std::string_view answer) noexcept -> bool;
[[nodiscard]] auto Prompt() -> PromptChoice;
// An empty result means the debugger was launched and has exited. The original
// crashed case remains a ztest failure regardless of the result of its replay.
[[nodiscard]] auto Launch(const Replay& replay) -> std::optional<std::string>;

} // namespace Ztest::Debugger
