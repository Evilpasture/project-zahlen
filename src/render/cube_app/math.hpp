#include <array>
#include <cmath>

// ----------------------------------------------------------------------------
// Column-Major, Column-Vector Math Library (P * V * M)
// ----------------------------------------------------------------------------
struct Mat4 {
	std::array<float, 16> data;
};

static Mat4 Identity() noexcept {
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

static Mat4 LookAt(const std::array<float, 3>& eye, const std::array<float, 3>& center,
				   const std::array<float, 3>& up) noexcept {
	const std::array<float, 3> f = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
	const float f_len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	const std::array<float, 3> f_norm = {f[0] / f_len, f[1] / f_len, f[2] / f_len};

	const std::array<float, 3> s = {f_norm[1] * up[2] - f_norm[2] * up[1],
									f_norm[2] * up[0] - f_norm[0] * up[2],
									f_norm[0] * up[1] - f_norm[1] * up[0]};
	const float s_len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
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