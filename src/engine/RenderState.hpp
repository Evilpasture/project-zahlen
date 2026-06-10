#pragma once

#include <cstdint>

struct TAAState {
	bool enabled = true;
	float feedback = 0.95f; // 95% History, 5% Current Frame
	float jitterX = 0.0f;
	float jitterY = 0.0f;
	float prevJitterX = 0.0f;
	float prevJitterY = 0.0f;
	uint32_t frameIndex = 0; // Drives the Halton Jitter sequence
};

static TAAState g_TAAState;
