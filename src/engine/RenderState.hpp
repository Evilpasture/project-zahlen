#pragma once

#include <cstdint>

struct TAAState {
	bool enabled = true;
	float feedback = 0.95f;	 // 95% History, 5% Current Frame
	uint32_t frameIndex = 0; // Drives the Halton Jitter sequence
};

static TAAState g_TAAState;