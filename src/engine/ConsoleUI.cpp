// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Console.hpp>
#include <Zahlen/Scripting.hpp>
#include <detail/ControlFlow.hpp>
#include <imgui.h>
#include <print>
#include <string>
#include <threading/Mutex.hpp>
#include <vector>
std::vector<std::string> s_InvShellLog;
bool s_InvScrollToBottom = false;
namespace ZHLN {

namespace {

struct ConsoleEntryInternal {
	std::string text;
	ColorRGBA color;
};

static std::vector<std::string> s_History;
static int s_HistoryPos = -1;
static std::vector<ConsoleEntryInternal> s_Entries;
static ZHLN::Mutex s_Mutex;
static bool s_ScrollToBottom = false;

// Parses Unix ANSI escape codes and translates them to ImGui Colors [3]
void TextAnsi(const std::string& line) {
	ImVec4 color = ImVec4(0.9f, 0.9f, 0.9f, 1.0f); // Default off-white
	size_t pos = 0;
	bool hasPrinted = false; // Stably tracks if we have drawn text yet on this row [3]

	while (pos < line.size()) {
		size_t esc = line.find("\x1b[", pos);
		if (esc == std::string::npos) {
			if (hasPrinted) {
				ImGui::SameLine(0.0f, 0.0f);
			}
			ImGui::TextColored(color, "%s", line.substr(pos).c_str());
			break;
		}

		if (esc > pos) {
			if (hasPrinted) {
				ImGui::SameLine(0.0f, 0.0f);
			}
			ImGui::TextColored(color, "%s", line.substr(pos, esc - pos).c_str());
			hasPrinted = true; // Only flag true after text is actually drawn [3]
		}

		size_t m = line.find('m', esc);
		if (m == std::string::npos) {
			if (hasPrinted) {
				ImGui::SameLine(0.0f, 0.0f);
			}
			ImGui::TextColored(color, "%s", line.substr(esc).c_str());
			break;
		}

		std::string code = line.substr(esc + 2, m - (esc + 2));
		if (code == "0" || code == "39") {
			color = ImVec4(0.9f, 0.9f, 0.9f, 1.0f); // Reset
		} else if (code == "31") {
			color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red
		} else if (code == "32") {
			color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); // Green
		} else if (code == "33") {
			color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f); // Yellow
		} else if (code == "36") {
			color = ImVec4(0.3f, 0.8f, 1.0f, 1.0f); // Cyan
		}

		pos = m + 1;
	}
}

} // namespace

// ============================================================================
// GameConsole Implementation
// ============================================================================

void GameConsole::Log(std::string_view msg, ColorRGBA color) {
	ZHLN_LOCK(s_Mutex) {
		s_Entries.push_back({.text = std::string(msg), .color = color});
		s_ScrollToBottom = true;
	}
}

bool GameConsole::ConsumeScroll() {
	ZHLN_LOCK(s_Mutex) {
		bool s = s_ScrollToBottom;
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
#include <print>
	}
}

void GameConsole::GetEntry(size_t index, std::string_view& outText, float& outR, float& outG,
						   float& outB, float& outA) noexcept {
	ZHLN_LOCK(s_Mutex) {
		if (index < s_Entries.size()) {
			outText = s_Entries[index].text;
			outR = s_Entries[index].color.r;
			outG = s_Entries[index].color.g;
			outB = s_Entries[index].color.b;
			outA = s_Entries[index].color.a;
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

// ============================================================================
// UI Render Methods
// ============================================================================

int ConsoleInputCallback(ImGuiInputTextCallbackData* data) {
	if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
		size_t historyCount = GameConsole::GetHistoryCount();
		int& pos = GameConsole::HistoryPos();
		int prev_pos = pos;

		if (data->EventKey == ImGuiKey_UpArrow) {
			if (pos == -1) {
				pos = (int)historyCount - 1;
			} else if (pos > 0) {
				pos--;
			}
		} else if (data->EventKey == ImGuiKey_DownArrow) {
			if (pos != -1) {
				if (++pos >= (int)historyCount) {
					pos = -1;
				}
			}
		}

		if (prev_pos != pos) {
			std::string_view history_str = (pos >= 0) ? GameConsole::GetHistoryItem(pos) : "";
			data->DeleteChars(0, data->BufTextLen);
			data->InsertChars(0, history_str.data(), history_str.data() + history_str.size());
		}
	}
	return 0;
}

void DrawConsole(ScriptRunner& scriptRunner) {
	ImGui::SetNextWindowSize({520, 400}, ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Lua Console")) {
		ImGui::End();
		return;
	}

	if (ImGui::Button("Provoke GPU Hang")) {
		if (auto* engine = GetEngineContext()) {
			engine->ProvokeDeviceLost();
		}
	}
	ImGui::SameLine();

	const float footer_height =
		ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height), false);

	size_t entryCount = GameConsole::GetEntryCount();
	for (size_t i = 0; i < entryCount; ++i) {
		std::string_view text;
		float r;
		float g;
		float b;
		float a;
		GameConsole::GetEntry(i, text, r, g, b, a);
		ImGui::TextColored(ImVec4(r, g, b, a), "%.*s", (int)text.size(), text.data());
	}

	if (GameConsole::ConsumeScroll()) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
	ImGui::Separator();

	static char InputBuf[256] = "";
	if (ImGui::InputText("##InvInput", InputBuf, IM_ARRAYSIZE(InputBuf),
						 ImGuiInputTextFlags_EnterReturnsTrue)) {
		std::string cmd = InputBuf;
		if (!cmd.empty()) {
			s_InvShellLog.push_back("$ " + cmd);
			s_InvScrollToBottom = true;

			// Stream the input command to your physical system stdout [3]
			std::println("[InvShell Input] $ {}", cmd);

			// Construct the dynamic Lua callback
			std::string luaCall = "run_inventory_command('" + cmd + "')";
			scriptRunner.ExecuteString(luaCall);
		}
		InputBuf[0] = '\0';
		ImGui::SetKeyboardFocusHere(-1); // Automatically re-focus terminal prompt
	}

	ImGui::End();
}

void DrawInventoryShell(ScriptRunner& scriptRunner) {
	ImGui::SetNextWindowSize({500, 350}, ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Inventory Terminal (Subshell)")) {
		ImGui::End();
		return;
	}

	// Print welcome header on initial start
	if (s_InvShellLog.empty()) {
		s_InvShellLog.emplace_back("Inventory Subshell v1.0.0");
		s_InvShellLog.emplace_back(
			"Type 'ls' to view files, 'cd <dir>' to navigate, 'cat <file>' to read.");
		s_InvShellLog.emplace_back(
			"------------------------------------------------------------------");
	}

	const float footer_height =
		ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("InvScrollingRegion", ImVec2(0, -footer_height), false);

	for (const auto& line : s_InvShellLog) {
		if (line.starts_with("$ ")) {
			ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s",
							   line.c_str()); // Commands in Cyan
		} else {
			TextAnsi(line); // Beautiful out-of-line ANSI parsing!
		}
	}

	if (s_InvScrollToBottom) {
		ImGui::SetScrollHereY(1.0f);
		s_InvScrollToBottom = false;
	}
	ImGui::EndChild();
	ImGui::Separator();

	static char InputBuf[256] = "";
	if (ImGui::InputText("##InvInput", InputBuf, IM_ARRAYSIZE(InputBuf),
						 ImGuiInputTextFlags_EnterReturnsTrue)) {
		std::string cmd = InputBuf;
		if (!cmd.empty()) {
			s_InvShellLog.push_back("$ " + cmd);
			s_InvScrollToBottom = true;

			// Stream input command to system stdout and force immediate flush [3]
			std::println(stdout, "[InvShell Input] $ {}", cmd);
			std::fflush(stdout);

			// Construct the dynamic Lua callback
			std::string luaCall = "run_inventory_command('" + cmd + "')";
			scriptRunner.ExecuteString(luaCall);
		}
		InputBuf[0] = '\0';
		ImGui::SetKeyboardFocusHere(-1); // Automatically re-focus terminal prompt
	}
	ImGui::End();
}
} // namespace ZHLN
