#pragma once
#include <Utils.hpp>
#include <cmath>
#include <cstdint>
#include <vector>


namespace ZHLN::Texture {

// Generates a Test Pattern (Grid with a colorful center)
template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateTest() {
	std::vector<uint32_t> pixels(Width * Height);

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		// ABGR little-endian (matches VK_FORMAT_R8G8B8A8_UNORM on Apple/Windows)
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	const float fW = static_cast<float>(Width);
	const float fH = static_cast<float>(Height);
	const float cx = fW * 0.5f;
	const float cy = fH * 0.5f;

	const float radius = ZHLN::Min(fW, fH) * 0.15625f;
	const float radiusSq = radius * radius;

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

			float diff = dSq - radiusSq;
			if (diff * diff < (radiusSq * 4.0f) && !inBand) {
				float d = std::sqrt(dSq);
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

template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateTVInterrupt() {
	std::vector<uint32_t> pixels(Width * Height);

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

// Tests normal map sampling & tangent-space lighting pipelines.
template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateBrickNormalMap() {
	std::vector<uint32_t> pixels(Width * Height);

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	const uint32_t brickW = Width / 4;
	const uint32_t brickH = Height / 8;
	const uint32_t mortarW = ZHLN::Max(Width / 64u, 2u);
	const uint32_t mortarH = ZHLN::Max(Height / 64u, 2u);

	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			uint32_t row = y / brickH;
			uint32_t offset = (row % 2 == 0) ? 0 : brickW / 2;
			uint32_t localX = (x + offset) % brickW;
			uint32_t localY = y % brickH;

			bool inMortarX = localX < mortarW || localX >= brickW - mortarW;
			bool inMortarY = localY < mortarH || localY >= brickH - mortarH;
			bool inMortar = inMortarX || inMortarY;

			uint8_t nx = 128, ny = 128, nz = 255;

			if (inMortar) {
				float bevelX = 0.0f, bevelY = 0.0f;
				if (inMortarX)
					bevelX = (localX < mortarW) ? +1.0f : -1.0f;
				if (inMortarY)
					bevelY = (localY < mortarH) ? +1.0f : -1.0f;

				float len = std::sqrt(bevelX * bevelX + bevelY * bevelY + 1.0f);
				nx = uint8_t((bevelX / len) * 127.0f + 128.0f);
				ny = uint8_t((bevelY / len) * 127.0f + 128.0f);
				nz = uint8_t((1.0f / len) * 127.0f + 128.0f);
			}

			pixels[y * Width + x] = Pack(nx, ny, nz);
		}
	}
	return pixels;
}

// Procedural marble — tests smooth noise blending and color gradient sampling.
template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateMarble() {
	std::vector<uint32_t> pixels(Width * Height);

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	auto noise = [](float x, float y) -> float {
		return std::sin(x * 1.7f + std::sin(y * 2.3f + std::sin(x * 0.9f))) +
			   std::sin(y * 2.1f + std::sin(x * 3.1f)) * 0.5f;
	};

	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			float u = float(x) / float(Width);
			float v = float(y) / float(Height);

			float n = noise(u * 6.0f, v * 6.0f);
			float vein = std::abs(std::sin(u * 3.14159f * 3.0f + n * 2.5f));

			uint8_t base = uint8_t(180.0f + vein * 75.0f);
			uint8_t grey = uint8_t(160.0f + vein * 90.0f);

			pixels[y * Width + x] = Pack(base, grey, base);
		}
	}
	return pixels;
}

template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateMarbleCrisp() {
	std::vector<uint32_t> pixels(Width * Height);

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	// --- Math Core ---
	auto Hash = [](float x, float y) {
		float d = x * 12.9898f + y * 78.233f;
		return ZHLN::Fract(std::sin(d) * 43758.5453123f);
	};

	auto Noise = [&](float x, float y) {
		float ix = std::floor(x);
		float iy = std::floor(y);
		float fx = ZHLN::Fract(x);
		float fy = ZHLN::Fract(y);

		float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
		float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

		return ZHLN::Mix(ZHLN::Mix(Hash(ix, iy), Hash(ix + 1.0f, iy), ux),
						 ZHLN::Mix(Hash(ix, iy + 1.0f), Hash(ix + 1.0f, iy + 1.0f), ux), uy);
	};

	auto FBM = [&](float x, float y, int octaves) {
		float val = 0.0f;
		float amp = 0.5f;
		for (int i = 0; i < octaves; i++) {
			val += amp * Noise(x, y);
			x *= 2.1f;
			y *= 2.15f;
			amp *= 0.5f;
		}
		return val;
	};

	// --- Main Generator ---
	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			float u = (float)x / Width * 3.0f;
			float v = (float)y / Height * 3.0f;

			float qx = FBM(u, v, 4);
			float qy = FBM(u + 1.2f, v + 3.1f, 4);

			float rx = FBM(u + 4.0f * qx + 1.7f, v + 4.0f * qy + 9.2f, 4);
			float ry = FBM(u + 4.0f * qx + 8.3f, v + 4.0f * qy + 2.8f, 4);

			float pattern = FBM(u + 4.0f * rx, v + 4.0f * ry, 4);

			float veinBase = FBM(u * 2.0f + rx, v * 2.0f + ry, 3);
			float vein = 1.0f - std::abs(std::sin(veinBase * 10.0f + pattern * 2.0f));
			vein = std::pow(vein, 12.0f);

			float grain = Hash((float)x, (float)y) * 0.12f;

			float r_map = ZHLN::Mix(0.95f, 0.40f, pattern);
			float g_map = ZHLN::Mix(0.95f, 0.42f, pattern);
			float b_map = ZHLN::Mix(0.98f, 0.45f, pattern);

			r_map = ZHLN::Mix(r_map, 0.1f, vein);
			g_map = ZHLN::Mix(g_map, 0.1f, vein);
			b_map = ZHLN::Mix(b_map, 0.12f, vein);

			r_map += grain;
			g_map += grain;
			b_map += grain;

			pixels[y * Width + x] = Pack((uint8_t)(ZHLN::Clamp(r_map, 0, 1) * 255),
										 (uint8_t)(ZHLN::Clamp(g_map, 0, 1) * 255),
										 (uint8_t)(ZHLN::Clamp(b_map, 0, 1) * 255));
		}
	}
	return pixels;
}

// Mandelbrot set — stress-tests texture coordinate precision & mip filtering.
template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateMandelbrot() {
	std::vector<uint32_t> pixels(Width * Height);

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	const int maxIter = 128;

	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			float cr = (float(x) / float(Width)) * 3.5f - 2.5f;
			float ci = (float(y) / float(Height)) * 2.5f - 1.25f;

			float zr = 0.0f, zi = 0.0f;
			int iter = 0;
			while (iter < maxIter && zr * zr + zi * zi < 4.0f) {
				float tmp = zr * zr - zi * zi + cr;
				zi = 2.0f * zr * zi + ci;
				zr = tmp;
				++iter;
			}

			uint8_t r, g, b;
			if (iter == maxIter) {
				r = g = b = 0;
			} else {
				float t = float(iter) / float(maxIter);
				r = uint8_t(9 * (1 - t) * t * t * t * 255);
				g = uint8_t(15 * (1 - t) * (1 - t) * t * t * 255);
				b = uint8_t(8.5f * (1 - t) * (1 - t) * (1 - t) * t * 255 + t * 255);
			}

			pixels[y * Width + x] = Pack(r, g, b);
		}
	}
	return pixels;
}

// Radial colour wheel — tests hue/saturation rendering and polar UV math.
template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateColorWheel() {
	std::vector<uint32_t> pixels(Width * Height);

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	auto hsv2rgb = [](float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
		auto f = [&](float n) {
			float k = ZHLN::Fract(n + h * 6.0f);
			float m = ZHLN::Min(k, 4.0f - k);
			m = ZHLN::Min(m, 1.0f);
			m = ZHLN::Max(m, 0.0f);
			return v * (1.0f - s * m);
		};

		r = uint8_t(f(5.0f) * 255.0f);
		g = uint8_t(f(3.0f) * 255.0f);
		b = uint8_t(f(1.0f) * 255.0f);
	};

	const float cx = float(Width) * 0.5f;
	const float cy = float(Height) * 0.5f;
	const float maxR = ZHLN::Min(cx, cy);

	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			float dx = float(x) - cx;
			float dy = float(y) - cy;
			float dist = std::sqrt(dx * dx + dy * dy);

			if (dist > maxR) {
				pixels[y * Width + x] = Pack(30, 30, 30);
				continue;
			}

			float angle = std::atan2(dy, dx);
			float hue = (angle + 3.14159265f) / (2.0f * 3.14159265f);
			float sat = dist / maxR;
			float val = 1.0f;

			uint8_t r, g, b;
			hsv2rgb(hue, sat, val, r, g, b);
			pixels[y * Width + x] = Pack(r, g, b);
		}
	}
	return pixels;
}

} // namespace ZHLN::Texture