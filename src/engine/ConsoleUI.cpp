#include <Zahlen/Console.hpp>
#include <Zahlen/Scripting.hpp>
#include <detail/ControlFlow.hpp>
#include <imgui.h>
#include <string>
#include <threading/Mutex.hpp>
#include <vector>

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
			if (pos == -1)
				pos = (int)historyCount - 1;
			else if (pos > 0)
				pos--;
		} else if (data->EventKey == ImGuiKey_DownArrow) {
			if (pos != -1) {
				if (++pos >= (int)historyCount)
					pos = -1;
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

	const float footer_height =
		ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height), false);

	size_t entryCount = GameConsole::GetEntryCount();
	for (size_t i = 0; i < entryCount; ++i) {
		std::string_view text;
		float r, g, b, a;
		GameConsole::GetEntry(i, text, r, g, b, a);
		ImGui::TextColored(ImVec4(r, g, b, a), "%.*s", (int)text.size(), text.data());
	}

	if (GameConsole::ConsumeScroll()) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
	ImGui::Separator();

	static char InputBuf[256] = "";
	const ImGuiInputTextFlags input_flags =
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
	if (ImGui::InputText("##Input", InputBuf, IM_ARRAYSIZE(InputBuf), input_flags,
						 &ConsoleInputCallback)) {
		std::string cmd = InputBuf;
		if (!cmd.empty()) {
			GameConsole::Log("> " + cmd, {.r = 0.6f, .g = 0.6f, .b = 0.6f, .a = 1.0f});

			GameConsole::AddHistory(cmd);

			scriptRunner.ExecuteString(cmd);
		}
		InputBuf[0] = '\0';
		ImGui::SetKeyboardFocusHere(-1);
	}

	ImGui::End();
}

} // namespace ZHLN
