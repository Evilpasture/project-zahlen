#pragma once
#include <imgui.h>
#include <mutex>
#include <string>
#include <vector>

namespace ZHLN {
struct ConsoleEntry {
	std::string text;
	ImVec4 color;
};

class GameConsole {
  public:
	static void Log(const std::string& msg, ImVec4 color = {1, 1, 1, 1}) {
		std::lock_guard lock(m_Mutex);
		m_Entries.push_back({msg, color});
		m_ScrollToBottom = true;
	}

	static const std::vector<ConsoleEntry>& GetEntries() { return m_Entries; }
	static bool ConsumeScroll() {
		bool s = m_ScrollToBottom;
		m_ScrollToBottom = false;
		return s;
	}

  private:
	static inline std::vector<ConsoleEntry> m_Entries;
	static inline std::mutex m_Mutex;
	static inline bool m_ScrollToBottom = false;
};
} // namespace ZHLN