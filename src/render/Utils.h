#pragma once

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Raw C functions (Internal or for direct C use)
static inline int32_t zhln_clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static inline float zhln_clamp_f(float v, float lo, float hi) {
    return fminf(fmaxf(v, lo), hi);
}

static inline double zhln_clamp_d(double v, double lo, double hi) {
    return fmin(fmax(v, lo), hi);
}

#ifdef __cplusplus
} // extern "C"

// C++ Overloads: The C++ compiler picks the right one based on types
// This replaces the need for _Generic in C++ contexts.
static inline int32_t ZHLN_Clamp(int32_t v, int32_t lo, int32_t hi) { return zhln_clamp_i32(v, lo, hi); }
static inline float   ZHLN_Clamp(float v, float lo, float hi)     { return zhln_clamp_f(v, lo, hi); }
static inline double  ZHLN_Clamp(double v, double lo, double hi)    { return zhln_clamp_d(v, lo, hi); }

#else

// C Implementation: Use _Generic for dispatch
#define ZHLN_Clamp(v, lo, hi)                                                                      \
    _Generic((v),                                                                                  \
        float: zhln_clamp_f,                                                                       \
        double: zhln_clamp_d,                                                                      \
        int32_t: zhln_clamp_i32,                                                                    \
        default: zhln_clamp_i32)(v, lo, hi)
#endif