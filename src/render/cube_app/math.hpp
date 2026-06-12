// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <array>
#include <cmath>
#include <limits>

// ----------------------------------------------------------------------------
// Column-Major, Column-Vector Math Library (P * V * M)
// ----------------------------------------------------------------------------
struct Mat4 {
	std::array<float, 16> data;
};

static consteval Mat4 Identity() noexcept {
	Mat4 m{};
	m.data[0 * 4 + 0] = 1.0f;
	m.data[1 * 4 + 1] = 1.0f;
	m.data[2 * 4 + 2] = 1.0f;
	m.data[3 * 4 + 3] = 1.0f;
	return m;
}

static Mat4 Multiply(const Mat4& a, const Mat4& b) noexcept {
	Mat4 result{};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k)
				sum += a.data[k * 4 + r] * b.data[c * 4 + k];
			result.data[c * 4 + r] = sum;
		}
	}
	return result;
}

static Mat4 Perspective(float fov, float aspect, float znear, float zfar) noexcept {
	const float f = 1.0f / std::tan(fov * 0.5f);
	Mat4 m{};
	m.data[0 * 4 + 0] = f / aspect;
	m.data[1 * 4 + 1] = -f; // Vulkan Y-Flip
	m.data[2 * 4 + 2] = zfar / (znear - zfar);
	m.data[2 * 4 + 3] = -1.0f;
	m.data[3 * 4 + 2] = (znear * zfar) / (znear - zfar);
	m.data[3 * 4 + 3] = 0.0f;
	return m;
}

static Mat4 RotateX(float radians) noexcept {
	const float s = std::sin(radians);
	const float c = std::cos(radians);
	Mat4 m = Identity();
	m.data[1 * 4 + 1] = c;
	m.data[1 * 4 + 2] = s;
	m.data[2 * 4 + 1] = -s;
	m.data[2 * 4 + 2] = c;
	return m;
}

static Mat4 RotateY(float radians) noexcept {
	const float s = std::sin(radians);
	const float c = std::cos(radians);
	Mat4 m = Identity();
	m.data[0 * 4 + 0] = c;
	m.data[0 * 4 + 2] = -s;
	m.data[2 * 4 + 0] = s;
	m.data[2 * 4 + 2] = c;
	return m;
}

// Simple Newton-Raphson for constexpr sqrt
static constexpr float ConstSqrt(float x) {
	if (x == 0.0f) return 0.0f;
	if (x >= 0 && x < std::numeric_limits<float>::infinity()) {
		float curr = x, prev = 0;
		while (curr != prev) {
			prev = curr;
			curr = 0.5f * (curr + x / curr);
		}
		return curr;
	}
	return std::numeric_limits<float>::quiet_NaN();
}

static constexpr float BetterSqrt(float x) {
	if consteval {
		// Use the Newton-Raphson we discussed (it's pure)
		return ConstSqrt(x);
	} else {
		// Use the hardware-accelerated instruction (it's stateful/fast)
		return std::sqrt(x);
	}
}
static constexpr Mat4 LookAt(const std::array<float, 3>& eye, const std::array<float, 3>& center,
							 const std::array<float, 3>& up) noexcept {
	const std::array<float, 3> f = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
	const float f_len = BetterSqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	const std::array<float, 3> f_norm = {f[0] / f_len, f[1] / f_len, f[2] / f_len};

	const std::array<float, 3> s = {f_norm[1] * up[2] - f_norm[2] * up[1],
									f_norm[2] * up[0] - f_norm[0] * up[2],
									f_norm[0] * up[1] - f_norm[1] * up[0]};
	const float s_len = BetterSqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
	const std::array<float, 3> s_norm = {s[0] / s_len, s[1] / s_len, s[2] / s_len};

	const std::array<float, 3> u = {s_norm[1] * f_norm[2] - s_norm[2] * f_norm[1],
									s_norm[2] * f_norm[0] - s_norm[0] * f_norm[2],
									s_norm[0] * f_norm[1] - s_norm[1] * f_norm[0]};

	Mat4 m = Identity();
	m.data[0 * 4 + 0] = s_norm[0];
	m.data[1 * 4 + 0] = s_norm[1];
	m.data[2 * 4 + 0] = s_norm[2];
	m.data[0 * 4 + 1] = u[0];
	m.data[1 * 4 + 1] = u[1];
	m.data[2 * 4 + 1] = u[2];
	m.data[0 * 4 + 2] = -f_norm[0];
	m.data[1 * 4 + 2] = -f_norm[1];
	m.data[2 * 4 + 2] = -f_norm[2];

	m.data[3 * 4 + 0] = -(s_norm[0] * eye[0] + s_norm[1] * eye[1] + s_norm[2] * eye[2]);
	m.data[3 * 4 + 1] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
	m.data[3 * 4 + 2] = (f_norm[0] * eye[0] + f_norm[1] * eye[1] + f_norm[2] * eye[2]);
	return m;
}

static constexpr Mat4 Ortho(float left, float right, float bottom, float top, float znear,
							float zfar) {
	Mat4 m{};
	// X Scale
	m.data[0 * 4 + 0] = 2.0f / (right - left);

	// Y Scale (Negative for Vulkan's inverted Y-axis)
	m.data[1 * 4 + 1] = -2.0f / (top - bottom);

	// Z Scale (Negative for Right-Handed LookAt: geometry is at -Z)
	m.data[2 * 4 + 2] = -1.0f / (zfar - znear);

	// Translations
	m.data[3 * 4 + 0] = -(right + left) / (right - left);
	m.data[3 * 4 + 1] = (top + bottom) / (top - bottom); // Flipped translation for Y
	m.data[3 * 4 + 2] = -znear / (zfar - znear);
	m.data[3 * 4 + 3] = 1.0f;
	return m;
}

// 1. Verify Column-Major Storage via Identity
static_assert(Identity().data[0] == 1.0f && Identity().data[5] == 1.0f,
			  "Identity diagonal check failed");
static_assert(Identity().data[1] == 0.0f && Identity().data[4] == 0.0f,
			  "Storage is not contiguous or indexing is swapped");

// 2. Verify Right-Handed LookAt
// Eye at +10Z, looking at Origin.
// World (0,0,0) should become View (0,0,-10).
// In Column-Major, the translation part is data[12, 13, 14].
constexpr Mat4 view = LookAt({0, 0, 10}, {0, 0, 0}, {0, 1, 0});

static_assert(view.data[12] == 0.0f, "LookAt X-translation fail");
static_assert(view.data[13] == 0.0f, "LookAt Y-translation fail");
static_assert(view.data[14] == -10.0f, "LookAt Z-translation fail (Expected -10 for RH)");

// 3. Verify Basis Vectors (Rotation part of LookAt)
// Looking down -Z:
// The 's' (Right) vector should be +X: data[0,1,2] = {1,0,0}
// The 'u' (Up) vector should be +Y:    data[4,5,6] = {0,1,0}
// The '-f' (Forward) vector should be +Z: data[8,9,10] = {0,0,1}
static_assert(view.data[0] == 1.0f, "LookAt Right.x should be 1");
static_assert(view.data[5] == 1.0f, "LookAt Up.y should be 1");
static_assert(view.data[10] == 1.0f, "LookAt Back.z should be 1 (RH convention)");

// 4. Verify Vulkan Ortho [0, 1] Z-range
// Near plane at 0, Far plane at 1.
// A point at znear (0) should result in gl_Position.z = 0.0
// A point at zfar (1) should result in gl_Position.z = 1.0
constexpr Mat4 proj = Ortho(-1, 1, -1, 1, 0.0f, 1.0f);

// In Column-Major: data[10] is Z-scale, data[14] is Z-translation
// ViewSpace Z is negative in RH, so we check Z = -znear and Z = -zfar
// ClipZ = (ViewZ * m[10]) + m[14]
static_assert((0.0f * proj.data[10]) + proj.data[14] == 0.0f, "Ortho Near plane is not 0");
static_assert((-1.0f * proj.data[10]) + proj.data[14] == 1.0f, "Ortho Far plane is not 1");

// 5. Verify Vulkan Y-Inversion
// top = 1, bottom = -1.
// A point at Y=1 (top) should result in ClipY = -1.0 in Vulkan (Top of screen is -1)
static_assert((1.0f * proj.data[5]) + proj.data[13] == -1.0f, "Vulkan Y-axis is not inverted");
// ----------------------------------------------------------------------------
// CCW Cube Data
// ----------------------------------------------------------------------------
static constexpr std::array<int, 36> cube_indices = {
	0,	1,	2,	2,	3,	0,	// Front
	4,	5,	6,	6,	7,	4,	// Back
	8,	9,	10, 10, 11, 8,	// Top
	12, 13, 14, 14, 15, 12, // Bottom
	16, 17, 18, 18, 19, 16, // Right
	20, 21, 22, 22, 23, 20	// Left
};