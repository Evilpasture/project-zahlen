#pragma once
#include <detail/ControlFlow.hpp>
#include <imgui.h>
#include <string>
#include <threading/Mutex.hpp>
#include <vector>

namespace ZHLN {
struct ConsoleEntry {
	std::string text;
	ImVec4 color;
};

class GameConsole {
  public:
	static void Log(const std::string& msg, ImVec4 color = {1, 1, 1, 1}) {
		ZHLN_LOCK(m_Mutex) {
			m_Entries.push_back({msg, color});
			m_ScrollToBottom = true;
		}
	}

	static const std::vector<ConsoleEntry>& GetEntries() { return m_Entries; }
	static bool ConsumeScroll() {
		bool s = m_ScrollToBottom;
		m_ScrollToBottom = false;
		return s;
	}

	static void AddHistory(const std::string& cmd) {
		ZHLN_LOCK(m_Mutex) {
			// Don't add duplicate consecutive commands
			if (m_History.empty() || m_History.back() != cmd) {
				m_History.push_back(cmd);
			}
			m_HistoryPos = -1; // Reset position when new command added
		}
	}

	static const std::vector<std::string>& GetHistory() { return m_History; }

	static int& HistoryPos() { return m_HistoryPos; }

  private:
	static inline std::vector<std::string> m_History;
	static inline int m_HistoryPos = -1; // -1 means we are not navigating
	static inline std::vector<ConsoleEntry> m_Entries;
	static inline ZHLN::Mutex m_Mutex;
	static inline bool m_ScrollToBottom = false;
};
} // namespace ZHLN