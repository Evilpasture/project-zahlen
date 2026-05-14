#include <Zahlen/Console.hpp>
#include <Zahlen/Scripting.hpp>
#include <imgui.h>

namespace ZHLN {

// The callback function that ImGui calls whenever a key is pressed in the input box
int ConsoleInputCallback(ImGuiInputTextCallbackData* data) {
	if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
		const auto& history = GameConsole::GetHistory();
		int& pos = GameConsole::HistoryPos();
		int prev_pos = pos;

		if (data->EventKey == ImGuiKey_UpArrow) {
			if (pos == -1)
				pos = (int)history.size() - 1;
			else if (pos > 0)
				pos--;
		} else if (data->EventKey == ImGuiKey_DownArrow) {
			if (pos != -1) {
				if (++pos >= (int)history.size())
					pos = -1;
			}
		}

		// If the position changed, update the text buffer
		if (prev_pos != pos) {
			const char* history_str = (pos >= 0) ? history[pos].c_str() : "";
			data->DeleteChars(0, data->BufTextLen);
			data->InsertChars(0, history_str);
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

	for (const auto& entry : GameConsole::GetEntries()) {
		ImGui::TextColored(entry.color, "%s", entry.text.c_str());
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
			GameConsole::Log("> " + cmd, {0.6f, 0.6f, 0.6f, 1.0f});

			// NEW: Add to history
			GameConsole::AddHistory(cmd);

			scriptRunner.ExecuteString(cmd);
		}
		InputBuf[0] = '\0';
		ImGui::SetKeyboardFocusHere(-1);
	}

	ImGui::End();
}
} // namespace ZHLN