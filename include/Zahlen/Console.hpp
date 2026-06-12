// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <cstddef>
#include <string_view>

namespace ZHLN {

struct ColorRGBA {
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;
};

class GameConsole {
  public:
	static void Log(std::string_view msg, ColorRGBA color = {});

	static bool ConsumeScroll();

	static void AddHistory(std::string_view cmd);

	static int& HistoryPos();

	// High-performance, zero-allocation accessors for UI rendering
	static size_t GetEntryCount() noexcept;
	static void GetEntry(size_t index, std::string_view& outText, float& outR, float& outG,
						 float& outB, float& outA) noexcept;

	static size_t GetHistoryCount() noexcept;
	static std::string_view GetHistoryItem(size_t index) noexcept;
};

} // namespace ZHLN
