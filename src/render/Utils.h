#pragma once

#include <math.h>
#include <stdint.h>

// Internal helper for integers to avoid multiple evaluation of arguments
static inline int32_t zhln_clamp_i32(int32_t v, int32_t lo, int32_t hi) {
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

static inline float zhln_clamp_f(float v, float lo, float hi) {
	return fminf(fmaxf(v, lo), hi);
}

static inline double zhln_clamp_d(double v, double lo, double hi) {
	return fmin(fmax(v, lo), hi);
}

#define ZHLN_Clamp(v, lo, hi)                                                                      \
	_Generic((v),                                                                                  \
		float: zhln_clamp_f,                                                                       \
		double: zhln_clamp_d,                                                                      \
		int32_t: zhln_clamp_i32,                                                                   \
		default: zhln_clamp_i32)(v, lo, hi)