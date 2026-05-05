#pragma once
#include <Utils.hpp>
#include <array>
#include <cmath>
#include <cstdint>

// Generates a Test Pattern (Grid with a colorful center)
namespace ZHLN {
template <uint32_t Width, uint32_t Height>
inline const std::array<uint32_t, Width * Height> GenerateTestTexture() {
	std::array<uint32_t, Width * Height> pixels{};

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		// ABGR little-endian (matches VK_FORMAT_R8G8B8A8_UNORM on Apple/Windows)
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	const float fW = static_cast<float>(Width);
	const float fH = static_cast<float>(Height);
	const float cx = fW * 0.5f;
	const float cy = fH * 0.5f;

	const float radius = ZHLN::Min(fW, fH) * 0.15625f;
	const float radiusSq = radius * radius; // Use squared distance for constexpr compatibility

	const uint32_t cornerSize = ZHLN::Max(Width, Height) / 16;
	const uint32_t bandHalf = ZHLN::Max(Height / 16u, 1u);
	const uint32_t checkSize = ZHLN::Max(Width / 64u, 1u);

	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			float u = float(x) / (fW - 1.0f);
			float v = float(y) / (fH - 1.0f);

			// 1. Base UV gradient
			uint8_t r = static_cast<uint8_t>(u * 255.0f);
			uint8_t g = static_cast<uint8_t>(v * 255.0f);
			uint8_t b = static_cast<uint8_t>((1.0f - u * 0.5f - v * 0.5f) * 200.0f + 55.0f);

			// 2. Orientation Corner Markers
			bool inTL = x < cornerSize && y < cornerSize;
			bool inTR = x >= Width - cornerSize && y < cornerSize;
			bool inBL = x < cornerSize && y >= Height - cornerSize;
			bool inBR = x >= Width - cornerSize && y >= Height - cornerSize;

			if (inTL) {
				r = 255;
				g = 0;
				b = 0;
			} // Red
			else if (inTR) {
				r = 0;
				g = 255;
				b = 0;
			} // Green
			else if (inBL) {
				r = 0;
				g = 0;
				b = 255;
			} // Blue
			else if (inBR) {
				r = 255;
				g = 255;
				b = 0;
			} // Yellow

			// 3. Grid Logic
			bool gridMajX = (x % 64 == 0);
			bool gridMajY = (y % 64 == 0);
			bool gridMinX = (x % 16 == 0);
			bool gridMinY = (y % 16 == 0);

			if ((gridMinX || gridMinY) && !gridMajX && !gridMajY) {
				r = uint8_t(ZHLN::Max(0, int(r) - 20));
				g = uint8_t(ZHLN::Max(0, int(g) - 20));
				b = uint8_t(ZHLN::Max(0, int(b) - 20));
			}

			if (gridMajX || gridMajY) {
				r = g = b = 0;
			}

			// 4. Center Checkerboard Band
			bool inBand = (y + bandHalf >= cy) && (y < cy + bandHalf);
			if (inBand && !gridMajX && !gridMajY) {
				bool check = ((x / checkSize) +
							  ((y - static_cast<uint32_t>(cy - (float)bandHalf)) / checkSize)) %
								 2 ==
							 0;
				r = g = b = check ? 255 : 0;
			}

			// 5. Circle outline (Using squared distance to avoid sqrt in constexpr)
			float dx = float(x) - cx;
			float dy = float(y) - cy;
			float dSq = dx * dx + dy * dy;
			// Check if distance is roughly equal to radius (with 1.5px thickness)
			float diff = dSq - radiusSq;
			if (diff * diff < (radiusSq * 4.0f) && !inBand) { // Approximate ring
				// Refined check for thickness
				float d = std::sqrt(
					dSq); // Note: std::sqrt is constexpr since C++23 (or via compiler intrinsics)
				if (std::abs(d - radius) < 1.5f) {
					r = g = b = 255;
				}
			}

			// 6. Center Crosshair
			bool onH = std::abs(float(y) - cy) <= 1.0f &&
					   std::abs(float(x) - cx) < (float)cornerSize * 0.75f;
			bool onV = std::abs(float(x) - cx) <= 1.0f &&
					   std::abs(float(y) - cy) < (float)cornerSize * 0.75f;
			if (onH || onV) {
				r = g = b = 255;
			}

			pixels[y * Width + x] = Pack(r, g, b);
		}
	}
	return pixels;
}

template <uint32_t Width, uint32_t Height>
inline const std::array<uint32_t, Width * Height> GenerateTVInterruptTexture() {
	std::array<uint32_t, Width * Height> pixels{};

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	// Standard 75% SMPTE colors
	const uint32_t colors[7] = {
		Pack(192, 192, 192), // Gray
		Pack(192, 192, 0),	 // Yellow
		Pack(0, 192, 192),	 // Cyan
		Pack(0, 192, 0),	 // Green
		Pack(192, 0, 192),	 // Magenta
		Pack(192, 0, 0),	 // Red
		Pack(0, 0, 192),	 // Blue
	};

	for (uint32_t y = 0; y < Height; ++y) {
		float v = float(y) / float(Height);
		for (uint32_t x = 0; x < Width; ++x) {
			float u = float(x) / float(Width);
			int barIndex = int(u * 7);
			if (barIndex > 6)
				barIndex = 6;

			uint32_t finalColor;

			if (v < 0.67f) {
				finalColor = colors[barIndex];
			} else if (v < 0.75f) {
				const uint32_t rev[7] = {colors[6], Pack(16, 16, 16), colors[4], Pack(16, 16, 16),
										 colors[2], Pack(16, 16, 16), colors[0]};
				finalColor = rev[barIndex];
			} else {
				if (u < 1.0f / 6.0f)
					finalColor = Pack(0, 33, 76); // I-signal blue
				else if (u < 2.0f / 6.0f)
					finalColor = Pack(255, 255, 255); // White
				else if (u < 3.0f / 6.0f)
					finalColor = Pack(50, 0, 106); // Q-signal purple
				else
					finalColor = Pack(16, 16, 16); // Black
			}

			pixels[y * Width + x] = finalColor;
		}
	}
	return pixels;
}
} // namespace ZHLN