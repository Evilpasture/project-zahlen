// src/engine/SMAALUTGenerator.hpp

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace ZHLN::PBR {

struct SmaaVec2 {
	float x = 0.0f;
	float y = 0.0f;
	constexpr SmaaVec2() noexcept = default;
	constexpr SmaaVec2(float _x, float _y) noexcept : x(_x), y(_y) {}
	constexpr SmaaVec2 operator+(const SmaaVec2& o) const noexcept { return {x + o.x, y + o.y}; }
	constexpr SmaaVec2 operator-(const SmaaVec2& o) const noexcept { return {x - o.x, y - o.y}; }
	constexpr SmaaVec2 operator*(float s) const noexcept { return {x * s, y * s}; }
	constexpr SmaaVec2 operator/(float s) const noexcept { return {x / s, y / s}; }
	[[nodiscard]] SmaaVec2 Sqrt() const noexcept { return {std::sqrt(x), std::sqrt(y)}; }
};

inline SmaaVec2 Lerp(const SmaaVec2& a, const SmaaVec2& b, float p) noexcept {
	return a + (b - a) * p;
}

inline float Saturate(float a) noexcept {
	return std::clamp(a, 0.0f, 1.0f);
}

inline std::pair<SmaaVec2, SmaaVec2> SmoothArea(float d, SmaaVec2 a1, SmaaVec2 a2) noexcept {
	SmaaVec2 b1 = (a1 * 2.0f).Sqrt() * 0.5f;
	SmaaVec2 b2 = (a2 * 2.0f).Sqrt() * 0.5f;
	float p = Saturate(d / 32.0f); // SMOOTH_MAX_DISTANCE = 32
	return {Lerp(b1, a1, p), Lerp(b2, a2, p)};
}

inline std::pair<float, float> AreaOrthoInternal(SmaaVec2 p1, SmaaVec2 p2, float x) noexcept {
	float dx = p2.x - p1.x;
	float dy = p2.y - p1.y;
	float x1 = x;
	float x2 = x + 1.0f;
	float y1 = p1.y + dy * (x1 - p1.x) / dx;
	float y2 = p1.y + dy * (x2 - p1.x) / dx;

	bool inside = (x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x);
	if (inside) {
		bool sign1 = std::signbit(y1);
		bool sign2 = std::signbit(y2);
		bool is_trapezoid = (sign1 == sign2 || std::abs(y1) < 1e-4f || std::abs(y2) < 1e-4f);
		if (is_trapezoid) {
			float a = (y1 + y2) * 0.5f;
			return (a < 0.0f) ? std::pair{std::abs(a), 0.0f} : std::pair{0.0f, std::abs(a)};
		}
		float intersection_x = -p1.y * dx / dy + p1.x;
		float dummy = 0.0f;
		// Native C++ std::modf handles negative fractional parts identically to Python
		float frac = std::modf(intersection_x, &dummy);

		float a1 = (intersection_x > p1.x) ? y1 * frac * 0.5f : 0.0f;
		float a2 = (intersection_x < p2.x) ? y2 * (1.0f - frac) * 0.5f : 0.0f;
		float a = (std::abs(a1) > std::abs(a2)) ? a1 : -a2;
		return (a < 0.0f) ? std::pair{std::abs(a1), std::abs(a2)}
						  : std::pair{std::abs(a2), std::abs(a1)};
	}
	return {0.0f, 0.0f};
}

inline std::pair<float, float> AreaOrtho(int pattern, float left, float right,
										 float offset) noexcept {
	float d = left + right + 1.0f;
	float o1 = 0.5f + offset;
	float o2 = 0.5f + offset - 1.0f;

	switch (pattern) {
		case 0:
			return {0.0f, 0.0f};
		case 1:
			if (left <= right) {
				return AreaOrthoInternal(SmaaVec2(0.0f, o2), SmaaVec2(d * 0.5f, 0.0f), left);
			}
			return {0.0f, 0.0f};
		case 2:
			if (left >= right) {
				return AreaOrthoInternal(SmaaVec2(d * 0.5f, 0.0f), SmaaVec2(d, o2), left);
			}
			return {0.0f, 0.0f};
		case 3: {
			auto a1_pair = AreaOrthoInternal(SmaaVec2(0.0f, o2), SmaaVec2(d * 0.5f, 0.0f), left);
			auto a2_pair = AreaOrthoInternal(SmaaVec2(d * 0.5f, 0.0f), SmaaVec2(d, o2), left);
			auto smoothed =
				SmoothArea(d, {a1_pair.first, a1_pair.second}, {a2_pair.first, a2_pair.second});
			return {smoothed.first.x + smoothed.second.x, smoothed.first.y + smoothed.second.y};
		}
		case 4:
			if (left <= right) {
				return AreaOrthoInternal(SmaaVec2(0.0f, o1), SmaaVec2(d * 0.5f, 0.0f), left);
			}
			return {0.0f, 0.0f};
		case 5:
			return {0.0f, 0.0f};
		case 6:
			if (std::abs(offset) > 0.0f) {
				auto a1_p = AreaOrthoInternal(SmaaVec2(0.0f, o1), SmaaVec2(d, o2), left);
				auto a2_1 = AreaOrthoInternal(SmaaVec2(0.0f, o1), SmaaVec2(d * 0.5f, 0.0f), left);
				auto a2_2 = AreaOrthoInternal(SmaaVec2(d * 0.5f, 0.0f), SmaaVec2(d, o2), left);
				SmaaVec2 a1(a1_p.first, a1_p.second);
				SmaaVec2 a2 = SmaaVec2(a2_1.first, a2_1.second) + SmaaVec2(a2_2.first, a2_2.second);
				SmaaVec2 res = (a1 + a2) * 0.5f;
				return {res.x, res.y};
			}
			return AreaOrthoInternal(SmaaVec2(0.0f, o1), SmaaVec2(d, o2), left);
		case 7:
			return AreaOrthoInternal(SmaaVec2(0.0f, o1), SmaaVec2(d, o2), left);
		case 8:
			if (left >= right) {
				return AreaOrthoInternal(SmaaVec2(d * 0.5f, 0.0f), SmaaVec2(d, o1), left);
			}
			return {0.0f, 0.0f};
		case 9:
			if (std::abs(offset) > 0.0f) {
				auto a1_p = AreaOrthoInternal(SmaaVec2(0.0f, o2), SmaaVec2(d, o1), left);
				auto a2_1 = AreaOrthoInternal(SmaaVec2(0.0f, o2), SmaaVec2(d * 0.5f, 0.0f), left);
				auto a2_2 = AreaOrthoInternal(SmaaVec2(d * 0.5f, 0.0f), SmaaVec2(d, o1), left);
				SmaaVec2 a1(a1_p.first, a1_p.second);
				SmaaVec2 a2 = SmaaVec2(a2_1.first, a2_1.second) + SmaaVec2(a2_2.first, a2_2.second);
				SmaaVec2 res = (a1 + a2) * 0.5f;
				return {res.x, res.y};
			}
			return AreaOrthoInternal(SmaaVec2(0.0f, o2), SmaaVec2(d, o1), left);
		case 10:
			return {0.0f, 0.0f};
		case 11:
			return AreaOrthoInternal(SmaaVec2(0.0f, o2), SmaaVec2(d, o1), left);
		case 12: {
			auto a1_pair = AreaOrthoInternal(SmaaVec2(0.0f, o1), SmaaVec2(d * 0.5f, 0.0f), left);
			auto a2_pair = AreaOrthoInternal(SmaaVec2(d * 0.5f, 0.0f), SmaaVec2(d, o1), left);
			auto smoothed =
				SmoothArea(d, {a1_pair.first, a1_pair.second}, {a2_pair.first, a2_pair.second});
			return {smoothed.first.x + smoothed.second.x, smoothed.first.y + smoothed.second.y};
		}
		case 13:
			return AreaOrthoInternal(SmaaVec2(0.0f, o2), SmaaVec2(d, o1), left);
		case 14:
			return AreaOrthoInternal(SmaaVec2(0.0f, o1), SmaaVec2(d, o2), left);
		case 15:
		default:
			return {0.0f, 0.0f};
	}
}

inline float AreaDiagInternal1(SmaaVec2 p1, SmaaVec2 p2, SmaaVec2 p) noexcept {
	if (p1.x == p2.x && p1.y == p2.y) {
		return 1.0f;
	}
	float xm = (p1.x + p2.x) * 0.5f;
	float ym = (p1.y + p2.y) * 0.5f;
	float a = p2.y - p1.y;
	float b = p1.x - p2.x;
	float const_part = -a * xm - b * ym;

	float count = 0.0f;
	constexpr int samples = 30; // SAMPLES_DIAG
	constexpr float inv_samples = 1.0f / (float)(samples - 1);

	for (int x = 0; x < samples; ++x) {
		float px = p.x + (float)x * inv_samples;
		float a_px = a * px;
		for (int y = 0; y < samples; ++y) {
			float py = p.y + (float)y * inv_samples;
			if ((a_px + b * py + const_part) > 0.0f) {
				count += 1.0f;
			}
		}
	}
	return count / (float)(samples * samples);
}

static constexpr std::pair<int, int> edgesdiag[16] = {
	{0, 0}, {1, 0}, {0, 2}, {1, 2}, {2, 0}, {3, 0}, {2, 2}, {3, 2},
	{0, 1}, {1, 1}, {0, 3}, {1, 3}, {2, 1}, {3, 1}, {2, 3}, {3, 3}};

inline SmaaVec2 AreaDiagHelper(int pattern, SmaaVec2 p1, SmaaVec2 p2, float left,
							   SmaaVec2 offset) noexcept {
	int e1 = edgesdiag[pattern].first;
	int e2 = edgesdiag[pattern].second;
	p1 = (e1 > 0) ? p1 + offset : p1;
	p2 = (e2 > 0) ? p2 + offset : p2;
	float a1 = AreaDiagInternal1(p1, p2, SmaaVec2(1.0f, 0.0f) + SmaaVec2(left, left));
	float a2 = AreaDiagInternal1(p1, p2, SmaaVec2(1.0f, 1.0f) + SmaaVec2(left, left));
	return {1.0f - a1, a2};
}

inline SmaaVec2 AreaDiag(int pattern, float left, float right, SmaaVec2 offset) noexcept {
	float d = left + right + 1.0f;
	switch (pattern) {
		case 0: {
			SmaaVec2 a1 = AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 1: {
			SmaaVec2 a1 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(d, d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 2: {
			SmaaVec2 a1 =
				AreaDiagHelper(pattern, SmaaVec2(0.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 3:
			return AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left,
								  offset);
		case 4: {
			SmaaVec2 a1 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f), SmaaVec2(d, d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 5: {
			SmaaVec2 a1 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f), SmaaVec2(d, d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 6:
			return AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f), SmaaVec2(1.0f + d, d), left,
								  offset);
		case 7: {
			SmaaVec2 a1 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f), SmaaVec2(1.0f + d, d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 8: {
			SmaaVec2 a1 = AreaDiagHelper(pattern, SmaaVec2(0.0f, 0.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			SmaaVec2 a2 = AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 9:
			return AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, 1.0f + d), left,
								  offset);
		case 10: {
			SmaaVec2 a1 = AreaDiagHelper(pattern, SmaaVec2(0.0f, 0.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 11: {
			SmaaVec2 a1 = AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 12:
			return AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f), SmaaVec2(1.0f + d, 1.0f + d), left,
								  offset);
		case 13: {
			SmaaVec2 a1 = AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			SmaaVec2 a2 = AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 14: {
			SmaaVec2 a1 = AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
		case 15:
		default: {
			SmaaVec2 a1 = AreaDiagHelper(pattern, SmaaVec2(1.0f, 1.0f),
										 SmaaVec2(1.0f + d, 1.0f + d), left, offset);
			SmaaVec2 a2 =
				AreaDiagHelper(pattern, SmaaVec2(1.0f, 0.0f), SmaaVec2(1.0f + d, d), left, offset);
			return (a1 + a2) * 0.5f;
		}
	}
}

// ============================================================================
// Main Generator Entry Points (Safe direct-write to std::span)
// ============================================================================

inline void FillSmaaAreaTex(std::span<uint32_t> outPixels) noexcept {
	constexpr uint32_t width = 160;
	constexpr uint32_t height = 560;
	constexpr auto totalSize = static_cast<const size_t>(width) * height;

	// Bounds safety check
	if (outPixels.size() < totalSize) [[unlikely]] {
		return;
	}

	std::fill(outPixels.begin(), outPixels.begin() + totalSize, 0xFF000000u);

	constexpr float orthoOffsets[7] = {0.0f, -0.25f, 0.25f, -0.125f, 0.125f, -0.375f, 0.375f};
	constexpr SmaaVec2 diagOffsets[5] = {
		{0.00f, 0.00f}, {0.25f, -0.25f}, {-0.25f, 0.25f}, {0.125f, -0.125f}, {-0.125f, 0.125f}};

	static constexpr std::pair<int, int> edgesortho[16] = {
		{0, 0}, {3, 0}, {0, 3}, {3, 3}, {1, 0}, {4, 0}, {1, 3}, {4, 3},
		{0, 1}, {3, 1}, {0, 4}, {3, 4}, {1, 1}, {4, 1}, {1, 4}, {4, 4}};

	// 1. Bake Orthogonal Mappings (Left Half)
	for (int y = 0; y < 7; ++y) {
		float offset = orthoOffsets[y];
		uint32_t posY = 5 * 16 * y;

		for (int pattern = 0; pattern < 16; ++pattern) {
			uint32_t startX = 16 * edgesortho[pattern].first;
			uint32_t startY = posY + 16 * edgesortho[pattern].second;

			for (int right = 0; right < 16; ++right) {
				for (int left = 0; left < 16; ++left) {
					auto compLeft = static_cast<float>(left * left);
					auto compRight = static_cast<float>(right * right);

					auto res = AreaOrtho(pattern, compLeft, compRight, offset);

					auto r = static_cast<uint8_t>(Saturate(res.first) * 255.0f);
					auto g = static_cast<uint8_t>(Saturate(res.second) * 255.0f);

					outPixels[(startY + right) * width + (startX + left)] =
						0xFF000000u | (uint32_t(g) << 8) | r;
				}
			}
		}
	}

	// 2. Bake Diagonal Mappings (Right Half)
	for (int y = 0; y < 5; ++y) {
		SmaaVec2 offset = diagOffsets[y];
		uint32_t posY = 4 * 20 * y;

		for (int pattern = 0; pattern < 16; ++pattern) {
			uint32_t startX = 80 + 20 * edgesdiag[pattern].first;
			uint32_t startY = posY + 20 * edgesdiag[pattern].second;

			for (int right = 0; right < 20; ++right) {
				for (int left = 0; left < 20; ++left) {
					auto res = AreaDiag(pattern, (float)left, (float)right, offset);

					auto r = static_cast<uint8_t>(Saturate(res.x) * 255.0f);
					auto g = static_cast<uint8_t>(Saturate(res.y) * 255.0f);

					outPixels[(startY + right) * width + (startX + left)] =
						0xFF000000u | (uint32_t(g) << 8) | r;
				}
			}
		}
	}
}

inline void FillSmaaSearchTex(std::span<uint32_t> outPixels) noexcept {
	constexpr uint32_t width = 64;
	constexpr uint32_t height = 16;
	constexpr auto totalSize = static_cast<const size_t>(width) * height;

	// Bounds safety check
	if (outPixels.size() < totalSize) [[unlikely]] {
		return;
	}

	auto Bilinear = [](float e0, float e1, float e2, float e3) noexcept -> float {
		auto lerp = [](float v0, float v1, float p) { return v0 + (v1 - v0) * p; };
		float a = lerp(e0, e1, 1.0f - 0.25f);
		float b = lerp(e2, e3, 1.0f - 0.25f);
		return lerp(a, b, 1.0f - 0.125f);
	};

	struct EdgeCombo {
		std::array<int, 4> edges;
		float val;
	};
	std::array<EdgeCombo, 16> combos{};
	int idx = 0;
	for (int e0 : {0, 1}) {
		for (int e1 : {0, 1}) {
			for (int e2 : {0, 1}) {
				for (int e3 : {0, 1}) {
					combos[idx++] = {.edges = {e0, e1, e2, e3},
									 .val = Bilinear((float)e0, (float)e1, (float)e2, (float)e3)};
				}
			}
		}
	}

	auto find_edges = [&](float t) noexcept -> std::array<int, 4> {
		for (const auto& combo : combos) {
			if (std::abs(combo.val - t) < 1e-4f) {
				return combo.edges;
			}
		}
		return {0, 0, 0, 0};
	};

	auto deltaLeft = [](const std::array<int, 4>& left,
						const std::array<int, 4>& top) noexcept -> int {
		int d = 0;
		if (top[3] == 1) {
			d += 1;
		}
		if (d == 1 && top[2] == 1 && left[1] != 1 && left[3] != 1) {
			d += 1;
		}
		return d;
	};

	auto deltaRight = [](const std::array<int, 4>& left,
						 const std::array<int, 4>& top) noexcept -> int {
		int d = 0;
		if (top[3] == 1 && left[1] != 1 && left[3] != 1) {
			d += 1;
		}
		if (d == 1 && top[2] == 1 && left[0] != 1 && left[2] != 1) {
			d += 1;
		}
		return d;
	};

	// Intermediate workspace buffer
	std::array<uint8_t, static_cast<size_t>(66 * 33)> temp{};

	for (int x = 0; x < 33; ++x) {
		for (int y = 0; y < 33; ++y) {
			std::array<int, 4> edgesX = find_edges(0.03125f * x);
			std::array<int, 4> edgesY = find_edges(0.03125f * y);

			temp[y * 66 + x] = 127 * deltaLeft(edgesX, edgesY);
			temp[y * 66 + (33 + x)] = 127 * deltaRight(edgesX, edgesY);
		}
	}

	// Crop (0, 17, 64, 33) and Flip Vertically directly into output RGBA8
	for (uint32_t fy = 0; fy < height; ++fy) {
		uint32_t cy = 15 - fy;
		for (uint32_t fx = 0; fx < width; ++fx) {
			uint8_t val = temp[(17 + cy) * 66 + fx];
			outPixels[fy * width + fx] =
				0xFF000000u | (uint32_t(val) << 16) | (uint32_t(val) << 8) | val;
		}
	}
}

} // namespace ZHLN::PBR
