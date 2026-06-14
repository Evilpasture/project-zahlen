// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/SharedMath.hpp
#pragma once

#ifdef __cplusplus
// =========================================================================
// --- CPU Host Definitions (C++23) ---
// =========================================================================
#include <algorithm>
#include <cmath>
#include <functional>

// Jolt requires Jolt/Jolt.h to be included first
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>

// Directly expose C++ standard library scalar functions to match HLSL intrinsics
using std::abs;
using std::clamp;
using std::cos;
using std::exp; // Added for SoftClamp
using std::floor;
using std::lerp;
using std::max;
using std::min;
using std::pow;
using std::sin;
using std::sqrt;

#define INLINE_MATH inline

struct uint2;

struct bool2 {
	bool x, y;
};
struct bool3 {
	bool x, y, z;
};
struct bool4 {
	bool x, y, z, w;
};

// 1. Forward declarations
struct uint2;
struct float2;

// 2. Define uint2 (WITHOUT the float2 constructor body)
struct uint2 {
	union {
		struct {
			uint x, y;
		};
		struct {
			uint r{}, g{};
		};
	};
	constexpr uint2() : x(0), y(0) {}
	constexpr uint2(uint s) : x(s), y(s) {}
	constexpr uint2(uint _x, uint _y) : x(_x), y(_y) {}

	// Defer the implementation
	constexpr uint2(float2 v);
};

// 3. Define float2 (WITHOUT the uint2 constructor body)
struct float2 {
	union {
		struct {
			float x, y;
		};
		struct {
			float r{}, g{};
		};
	};
	constexpr float2() : x(0.0f), y(0.0f) {}
	constexpr float2(float s) : x(s), y(s) {}
	constexpr float2(float _x, float _y) : x(_x), y(_y) {}

	constexpr float2 operator+(float2 o) const { return {x + o.x, y + o.y}; }
	constexpr float2 operator-(float2 o) const { return {x - o.x, y - o.y}; }
	constexpr float2 operator*(float2 o) const { return {x * o.x, y * o.y}; }
	constexpr float2 operator/(float2 o) const { return {x / o.x, y / o.y}; }
	constexpr float2 operator*(float s) const { return {x * s, y * s}; }
	constexpr float2 operator/(float s) const { return {x / s, y / s}; }
	constexpr float2 operator-() const { return {-x, -y}; }
	constexpr float2& operator+=(float2 o) {
		x += o.x;
		y += o.y;
		return *this;
	}
	constexpr float2& operator-=(float2 o) {
		x -= o.x;
		y -= o.y;
		return *this;
	}
	constexpr float2& operator+=(float s) {
		x += s;
		y += s;
		return *this;
	}
	constexpr float2& operator-=(float s) {
		x -= s;
		y -= s;
		return *this;
	}
	constexpr float2& operator/=(float s) {
		x /= s;
		y /= s;
		return *this;
	}
	constexpr float2& operator*=(float s) {
		x *= s;
		y *= s;
		return *this;
	}

	// Defer the implementation
	constexpr float2(uint2 v);
};

// 4. Implement the conversions now that BOTH types are fully complete
constexpr uint2::uint2(float2 v) : x(static_cast<uint>(v.x)), y(static_cast<uint>(v.y)) {}

constexpr float2::float2(uint2 v) : x(static_cast<float>(v.x)), y(static_cast<float>(v.y)) {}

struct float3 {
	union {
		struct {
			float x, y, z;
		};
		struct {
			float r{}, g{}, b{};
		};
		float2 xy; // Fits directly at offset 0 (overlapping x, y)
	};
	constexpr float3() : x(0.0f), y(0.0f), z(0.0f) {}
	constexpr float3(float s) : x(s), y(s), z(s) {}
	constexpr float3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

	constexpr float3 operator+(float3 o) const { return {x + o.x, y + o.y, z + o.z}; }
	constexpr float3 operator-(float3 o) const { return {x - o.x, y - o.y, z - o.z}; }
	constexpr float3 operator*(float3 o) const { return {x * o.x, y * o.y, z * o.z}; }
	constexpr float3 operator/(float3 o) const { return {x / o.x, y / o.y, z / o.z}; }
	constexpr float3 operator*(float s) const { return {x * s, y * s, z * s}; }
	constexpr float3 operator/(float s) const { return {x / s, y / s, z / s}; }
	constexpr float3 operator-() const { return {-x, -y, -z}; }
	constexpr float3& operator+=(float3 o) {
		x += o.x;
		y += o.y;
		z += o.z;
		return *this;
	}
	constexpr float3& operator-=(float3 o) {
		x -= o.x;
		y -= o.y;
		z -= o.z;
		return *this;
	}
	constexpr float3& operator+=(float s) {
		x += s;
		y += s;
		z += s;
		return *this;
	}
	constexpr float3& operator-=(float s) {
		x -= s;
		y -= s;
		z -= s;
		return *this;
	}
	constexpr float3& operator/=(float s) {
		x /= s;
		y /= s;
		z /= s;
		return *this;
	}
	constexpr float3& operator*=(float s) {
		x *= s;
		y *= s;
		z *= s;
		return *this;
	}
};

struct float4 {
	union {
		struct {
			float x, y, z, w;
		};
		struct {
			float r{}, g{}, b{}, a{};
		};
		float2 xy;	// Overlaps x, y
		float3 xyz; // Overlaps x, y, z
		float3 rgb; // Overlaps r, g, b
	};
	constexpr float4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
	constexpr float4(float s) : x(s), y(s), z(s), w(s) {}
	constexpr float4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
	constexpr float4(float3 v, float _w) : x(v.x), y(v.y), z(v.z), w(_w) {}

	constexpr float4 operator+(float4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
	constexpr float4 operator-(float4 o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
	constexpr float4 operator*(float4 o) const { return {x * o.x, y * o.y, z * o.z, w * o.w}; }
	constexpr float4 operator/(float4 o) const { return {x / o.x, y / o.y, z / o.w, w / o.w}; }
	constexpr float4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
	constexpr float4 operator/(float s) const { return {x / s, y / s, z / s, w / s}; }
	constexpr float4& operator+=(float4 o) {
		x += o.x;
		y += o.y;
		z += o.z;
		w += o.w;
		return *this;
	}
	constexpr float4& operator-=(float4 o) {
		x -= o.x;
		y -= o.y;
		z -= o.z;
		w -= o.w;
		return *this;
	}
	constexpr float4& operator+=(float s) {
		x += s;
		y += s;
		z += s;
		w += s;
		return *this;
	}
	constexpr float4& operator-=(float s) {
		x -= s;
		y -= s;
		z -= s;
		w -= s;
		return *this;
	}
	constexpr float4& operator/=(float s) {
		x /= s;
		y /= s;
		z /= s;
		w /= s;
		return *this;
	}
	constexpr float4& operator*=(float s) {
		x *= s;
		y *= s;
		z *= s;
		w *= s;
		return *this;
	}
};

struct float3x3 {
	float3 r0, r1, r2;
	constexpr float3x3() : r0(), r1(), r2() {}
	constexpr float3x3(float3 _r0, float3 _r1, float3 _r2) : r0(_r0), r1(_r1), r2(_r2) {}
};

// Left-side scalar multiplication
inline float2 operator*(float s, float2 v) {
	return {s * v.x, s * v.y};
}
inline float3 operator*(float s, float3 v) {
	return {s * v.x, s * v.y, s * v.z};
}
inline float4 operator*(float s, float4 v) {
	return {s * v.x, s * v.y, s * v.z, s * v.w};
}

// --- Vector overloads & Vector-only intrinsics ---
inline float dot(float2 a, float2 b) {
	return a.x * b.x + a.y * b.y;
}
inline float dot(float3 a, float3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float dot(float4 a, float4 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

inline float3 cross(float3 a, float3 b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(float2 v) {
	return sqrt(dot(v, v));
}
inline float length(float3 v) {
	return sqrt(dot(v, v));
}
inline float length(float4 v) {
	return sqrt(dot(v, v));
}

inline float2 normalize(float2 v) {
	float len = length(v);
	return len > 0.0f ? v / len : float2(0.0f, 0.0f);
}
inline float3 normalize(float3 v) {
	float len = length(v);
	return len > 0.0f ? v / len : float3(0.0f, 0.0f, 0.0f);
}
inline float4 normalize(float4 v) {
	float len = length(v);
	return len > 0.0f ? v / len : float4(0.0f, 0.0f, 0.0f, 0.0f);
}

inline float2 clamp(float2 v, float2 minVal, float2 maxVal) {
	return {clamp(v.x, minVal.x, maxVal.x), clamp(v.y, minVal.y, maxVal.y)};
}
inline float3 clamp(float3 v, float3 minVal, float3 maxVal) {
	return {clamp(v.x, minVal.x, maxVal.x), clamp(v.y, minVal.y, maxVal.y),
			clamp(v.z, minVal.z, maxVal.z)};
}

inline float saturate(float v) {
	return clamp(v, 0.0f, 1.0f);
}
inline float2 saturate(float2 v) {
	return clamp(v, float2(0.0f), float2(1.0f));
}
inline float3 saturate(float3 v) {
	return clamp(v, float3(0.0f), float3(1.0f));
}

inline float2 max(float2 a, float2 b) {
	return {max(a.x, b.x), max(a.y, b.y)};
}
inline float3 max(float3 a, float3 b) {
	return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)};
}

inline float2 min(float2 a, float2 b) {
	return {min(a.x, b.x), min(a.y, b.y)};
}
inline float3 min(float3 a, float3 b) {
	return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)};
}

inline float3 reflect(float3 i, float3 n) {
	return i - 2.0f * dot(n, i) * n;
}
inline float3 lerp(float3 a, float3 b, float t) {
	return a + t * (b - a);
}

inline float step(float edge, float x) {
	return x < edge ? 0.0f : 1.0f;
}
inline float3 step(float3 edge, float3 x) {
	return {step(edge.x, x.x), step(edge.y, x.y), step(edge.z, x.z)};
}

inline float smoothstep(float edge0, float edge1, float x) {
	float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

inline float2 smoothstep(float edge0, float edge1, float2 x) {
	return {smoothstep(edge0, edge1, x.x), smoothstep(edge0, edge1, x.y)};
}

inline float3 smoothstep(float edge0, float edge1, float3 x) {
	return {smoothstep(edge0, edge1, x.x), smoothstep(edge0, edge1, x.y),
			smoothstep(edge0, edge1, x.z)};
}

// Optional: Vector-Vector bounds variants if your shaders use them
inline float2 smoothstep(float2 edge0, float2 edge1, float2 x) {
	return {smoothstep(edge0.x, edge1.x, x.x), smoothstep(edge0.y, edge1.y, x.y)};
}

inline float distance(float3 a, float3 b) {
	return length(a - b);
}

inline float frac(float x) {
	return x - floor(x);
}
inline float2 frac(float2 v) {
	return {frac(v.x), frac(v.y)};
}
inline float3 frac(float3 v) {
	return {frac(v.x), frac(v.y), frac(v.z)};
}

inline float2 abs(float2 v) {
	return {std::abs(v.x), std::abs(v.y)};
}
inline float3 abs(float3 v) {
	return {std::abs(v.x), std::abs(v.y), std::abs(v.z)};
}

inline bool2 operator<(float2 a, float b) {
	return {.x = a.x < b, .y = a.y < b};
}
inline bool2 operator>(float2 a, float b) {
	return {.x = a.x > b, .y = a.y > b};
}
inline bool2 operator<=(float2 a, float b) {
	return {.x = a.x <= b, .y = a.y <= b};
}
inline bool2 operator>=(float2 a, float b) {
	return {.x = a.x >= b, .y = a.y >= b};
}

inline bool any(bool2 v) {
	return v.x || v.y;
}
inline bool all(bool2 v) {
	return v.x && v.y;
}
inline bool any(bool3 v) {
	return v.x || v.y || v.z;
}
inline bool all(bool3 v) {
	return v.x && v.y && v.z;
}

// Map float4x4 to Jolt's highly optimized 4x4 matrix
using float4x4 = JPH::Mat44;

// Implement the HLSL matrix-vector multiplication helper
inline float4 mul(const float4x4& m, float4 v) {
	JPH::Vec4 r = m * JPH::Vec4(v.x, v.y, v.z, v.w);
	return {r.GetX(), r.GetY(), r.GetZ(), r.GetW()};
}

// Standard Texture2D definition
template <typename T> struct Texture2D {
	std::function<T(float2 uv, float mip)> sample_callback;
	uint32_t width = 1920;	// Default mock width
	uint32_t height = 1080; // Default mock height

	T SampleLevel(void* /*sampler*/, float2 uv, float mip) const {
		if (sample_callback) {
			return sample_callback(uv, mip);
		}
		return T{};
	}

	void GetDimensions(uint32_t& w, uint32_t& h) const {
		w = width;
		h = height;
	}
};

// Specialization for Texture2D<float> to support scalar swizzles (depth.r)
template <> struct Texture2D<float> {
	std::function<float(float2 uv, float mip)> sample_callback;
	uint32_t width = 1920;	// Default mock width
	uint32_t height = 1080; // Default mock height

	struct ScalarSwizzle {
		float r;
		operator float() const { return r; }
	};

	ScalarSwizzle SampleLevel(void* /*sampler*/, float2 uv, float mip) const {
		if (sample_callback) {
			return {sample_callback(uv, mip)};
		}
		return {0.0f};
	}

	void GetDimensions(uint32_t& w, uint32_t& h) const {
		w = width;
		h = height;
	}
};

template <typename T = float4> struct TextureCube {
	std::function<T(float3 dir, float mip)> sample_callback;

	T SampleLevel(void* /*sampler*/, float3 dir, float mip) const {
		if (sample_callback) {
			return sample_callback(dir, mip);
		}
		return T{};
	}
};

using SamplerState = void*;
#else
// =========================================================================
// --- GPU Device Definitions (HLSL) ---
// =========================================================================
#define INLINE_MATH
#endif
