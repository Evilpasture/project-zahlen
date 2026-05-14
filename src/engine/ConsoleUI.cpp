#include <Zahlen/Console.hpp>
#include <Zahlen/Scripting.hpp>
#include <imgui.h>

namespace ZHLN {
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
	if (ImGui::InputText("##Input", InputBuf, IM_ARRAYSIZE(InputBuf),
						 ImGuiInputTextFlags_EnterReturnsTrue)) {
		std::string cmd = InputBuf;
		GameConsole::Log("> " + cmd, {0.6f, 0.6f, 0.6f, 1.0f});

		scriptRunner.ExecuteString(cmd); // Tell Lua to run this

		InputBuf[0] = '\0';
		ImGui::SetKeyboardFocusHere(-1); // Keep focus on input
	}

	ImGui::End();
}
} // namespace ZHLN