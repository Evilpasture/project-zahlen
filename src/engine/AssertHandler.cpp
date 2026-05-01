#include "engine/Log.hpp"

#include <cstdarg>
#include <cstdio>

namespace ZHLN {

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void {
	va_list list;
	va_start(list, inFMT);

	// Use a local buffer for the C-style string formatting
	char buffer[1024];
	int result = std::vsnprintf(buffer, sizeof(buffer), inFMT, list);

	va_end(list);

	if (result > 0) {
		// Hand off the formatted string to our modern Log system
		// We use "{}" to ensure we don't re-parse the Jolt output for format specifiers
		ZHLN::Log("{}", buffer);
	}
}

auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
					  uint32_t inLine) noexcept -> bool {
	ZHLN::Log("--- JOLT ASSERT FAILED ---\n"
			  "Expr: {}\n"
			  "Msg:  {}\n"
			  "File: {}:{}\n"
			  "--------------------------\n",
			  inExpression, (inMessage ? inMessage : "None"), inFile, inLine);
	return true; // Trigger debug break
}

} // namespace ZHLN