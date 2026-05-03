#pragma once

#include <math.h>
#include <stdint.h>

// Internal helper for integers to avoid multiple evaluation of arguments
static inline int32_t _clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static inline float _clamp_f(float v, float lo, float hi) {
    return fminf(fmaxf(v, lo), hi);
}

static inline double _clamp_d(double v, double lo, double hi) {
    return fmin(fmax(v, lo), hi);
}

#define ZHLN_Clamp(v, lo, hi) _Generic((v), \
    float:  _clamp_f,                       \
    double: _clamp_d,                       \
    int32_t: _clamp_i32,                    \
    default: _clamp_i32                     \
)(v, lo, hi)