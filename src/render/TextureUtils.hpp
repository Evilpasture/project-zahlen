// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Utils.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN::Texture {

// Generates a Test Pattern (Grid with a colorful center)
template <uint32_t Width, uint32_t Height> inline auto GenerateTest() -> std::vector<uint32_t> {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		// ABGR little-endian (matches VK_FORMAT_R8G8B8A8_UNORM on Apple/Windows)
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	const auto fW = static_cast<float>(Width);
	const auto fH = static_cast<float>(Height);
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
			auto r = static_cast<uint8_t>(u * 255.0f);
			auto g = static_cast<uint8_t>(v * 255.0f);
			auto b = static_cast<uint8_t>((1.0f - u * 0.5f - v * 0.5f) * 200.0f + 55.0f);

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
			float dSq = (dx * dx) + (dy * dy);

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

			pixels[(y * Width) + x] = Pack(r, g, b);
		}
	}
	return pixels;
}

template <uint32_t Width, uint32_t Height>
inline auto GenerateTVInterrupt() -> std::vector<uint32_t> {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

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
			if (barIndex > 6) {
				barIndex = 6;
			}

			uint32_t finalColor = 0;

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

			pixels[(y * Width) + x] = finalColor;
		}
	}
	return pixels;
}

// Tests normal map sampling & tangent-space lighting pipelines.
template <uint32_t Width, uint32_t Height>
inline auto GenerateBrickNormalMap() -> std::vector<uint32_t> {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

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

			uint8_t nx = 128;
			uint8_t ny = 128;
			uint8_t nz = 255;

			if (inMortar) {
				float bevelX = 0.0f;
				float bevelY = 0.0f;
				if (inMortarX) {
					bevelX = (localX < mortarW) ? +1.0f : -1.0f;
				}
				if (inMortarY) {
					bevelY = (localY < mortarH) ? +1.0f : -1.0f;
				}

				float len = std::sqrt((bevelX * bevelX) + (bevelY * bevelY) + 1.0f);
				nx = uint8_t(((bevelX / len) * 127.0f) + 128.0f);
				ny = uint8_t(((bevelY / len) * 127.0f) + 128.0f);
				nz = uint8_t(((1.0f / len) * 127.0f) + 128.0f);
			}

			pixels[(y * Width) + x] = Pack(nx, ny, nz);
		}
	}
	return pixels;
}

// Procedural marble — tests smooth noise blending and color gradient sampling.
template <uint32_t Width, uint32_t Height> inline auto GenerateMarble() -> std::vector<uint32_t> {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			float u = float(x) / float(Width);
			float v = float(y) / float(Height);

			float n = Noise(u * 6.0f, v * 6.0f);
			float vein = Abs(Sin((u * 3.14159f * 3.0f) + (n * 2.5f)));

			auto base = uint8_t(180.0f + vein * 75.0f);
			auto grey = uint8_t(160.0f + vein * 90.0f);

			pixels[(y * Width) + x] = PackColor(base, grey, base);
		}
	}
	return pixels;
}

template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateMarbleCrisp() {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

	// --- Main Generator ---
	for (uint32_t y = 0; y < Height; ++y) {
		for (uint32_t x = 0; x < Width; ++x) {
			float u = (float)x / Width * 3.0f;
			float v = (float)y / Height * 3.0f;

			float qx = FBM(u, v, 4);
			float qy = FBM(u + 1.2f, v + 3.1f, 4);

			float rx = FBM(u + (4.0f * qx) + 1.7f, v + (4.0f * qy) + 9.2f, 4);
			float ry = FBM(u + (4.0f * qx) + 8.3f, v + (4.0f * qy) + 2.8f, 4);

			float pattern = FBM(u + (4.0f * rx), v + (4.0f * ry), 4);

			float veinBase = FBM((u * 2.0f) + rx, (v * 2.0f) + ry, 3);
			float vein = 1.0f - Abs(Sin((veinBase * 10.0f) + (pattern * 2.0f)));
			vein = Power(vein, 12.0f);

			float grain = Hash((float)x, (float)y) * 0.12f;

			float r_map = Mix(0.95f, 0.40f, pattern);
			float g_map = Mix(0.95f, 0.42f, pattern);
			float b_map = Mix(0.98f, 0.45f, pattern);

			r_map = Mix(r_map, 0.1f, vein);
			g_map = Mix(g_map, 0.1f, vein);
			b_map = Mix(b_map, 0.12f, vein);

			r_map += grain;
			g_map += grain;
			b_map += grain;

			pixels[(y * Width) + x] =
				PackColor((uint8_t)(Clamp(r_map, 0, 1) * 255), (uint8_t)(Clamp(g_map, 0, 1) * 255),
						  (uint8_t)(Clamp(b_map, 0, 1) * 255));
		}
	}
	return pixels;
}

struct Blade {
	float rootX, rootY;
	float height;
	float halfWidth;
	float tilt;
	float bend;
	float shade;
	float vitality; // 1.0 = lush green, 0.0 = dead/brown
};

// Helper for alpha blending 32-bit colors (Assuming ARGB or XRGB format)
inline auto BlendColors(uint32_t bg, float r, float g, float b, float alpha) -> uint32_t {
	uint8_t bgR = (bg >> 16) & 0xFF;
	uint8_t bgG = (bg >> 8) & 0xFF;
	uint8_t bgB = bg & 0xFF;

	auto outR = (uint8_t)((r * alpha * 255.0f) + (bgR * (1.0f - alpha)));
	auto outG = (uint8_t)((g * alpha * 255.0f) + (bgG * (1.0f - alpha)));
	auto outB = (uint8_t)((b * alpha * 255.0f) + (bgB * (1.0f - alpha)));

	return PackColor(outR, outG, outB);
}

static auto GenerateOrganicBlades(uint32_t W, uint32_t H) -> std::vector<Blade> {
	std::vector<Blade> blades;
	float density = 1.5f; // Adjust for thicker/thinner grass fields

	// We still iterate over a grid to ensure coverage, but we place RANDOM amounts
	// of blades per cell based on noise, destroying the "grid" look.
	int cellSize = 8;

	for (int cy = -cellSize; cy < (int)H + cellSize; cy += cellSize) {
		for (int cx = -cellSize; cx < (int)W + cellSize; cx += cellSize) {

			// Clumping noise: High values = dense tufts, Low values = sparse/dirt
			float clumpNoise = FBM((float)cx * 0.005f, (float)cy * 0.005f, 3);
			int bladesInCell = (int)(clumpNoise * clumpNoise * 15.0f * density);

			for (int i = 0; i < bladesInCell; ++i) {
				float px = (float)cx + Hash((float)cx + i, (float)cy) * cellSize;
				float py = (float)cy + Hash((float)cx, (float)cy + i) * cellSize;

				// Wind Flow: Grass bends in cohesive waves, not randomly
				float windTilt = (FBM(px * 0.01f, py * 0.01f, 2) - 0.5f) * 2.0f;
				float windBend = (FBM(px * 0.015f + 10.0f, py * 0.015f, 2) - 0.5f) * 1.5f;

				Blade b;
				b.rootX = px;
				b.rootY = py;

				// Vitality: 1.0 = alive, 0.0 = dead. Driven by noise so patches die together.
				b.vitality = Clamp(FBM(px * 0.02f, py * 0.02f, 2) + 0.2f, 0.0f, 1.0f);

				// Dead grass is usually shorter and thinner
				float baseHeight = 20.0f + Hash(px, py) * 25.0f;
				b.height = baseHeight * (0.5f + b.vitality * 0.5f);
				b.halfWidth = (1.5f + Hash(px * 2.0f, py * 2.0f)) * (0.6f + b.vitality * 0.4f);

				b.tilt =
					windTilt + (Hash(px, py * 3.0f) - 0.5f) * 0.3f; // Macro wind + micro jitter
				b.bend = windBend + (Hash(px * 3.0f, py) - 0.5f) * 0.3f;
				b.shade = 0.7f + Hash(px, py) * 0.3f; // Slight lightness variation

				blades.push_back(b);
			}
		}
	}
	return blades;
}

template <uint32_t Width, uint32_t Height> inline std::vector<uint32_t> GenerateGrassTexture() {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

	// 1. Fill background with an organic dirt/moss base (prevents spotty black holes)
	for (uint32_t py = 0; py < Height; ++py) {
		for (uint32_t px = 0; px < Width; ++px) {
			float dirtNoise = FBM(px * 0.02f, py * 0.02f, 3);
			float r = Lerp(0.15f, 0.10f, dirtNoise);
			float g = Lerp(0.12f, 0.08f, dirtNoise);
			float b = Lerp(0.08f, 0.05f, dirtNoise);
			pixels[py * Width + px] =
				PackColor((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
		}
	}

	// 2. Generate and sort blades (Back to Front)
	std::vector<Blade> blades = GenerateOrganicBlades(Width, Height);
	std::sort(blades.begin(), blades.end(),
			  [](const Blade& a, const Blade& b) { return a.rootY < b.rootY; });

	// 3. Paint blades
	for (const auto& b : blades) {
		int startY = Clamp((int)(b.rootY - b.height * 1.5f), 0, (int)Height - 1);
		int endY = Clamp((int)(b.rootY + 2), 0, (int)Height - 1);

		float maxOffset = Abs(b.tilt * b.height) + Abs(b.bend * b.height) + b.halfWidth;
		int startX = Clamp((int)(b.rootX - maxOffset - 2), 0, (int)Width - 1);
		int endX = Clamp((int)(b.rootX + maxOffset + 2), 0, (int)Width - 1);

		for (int py = startY; py <= endY; ++py) {
			float relY = (float)py - b.rootY;
			if (relY > 0) {
				continue;
			}

			float t = Clamp(-relY / b.height, 0.0f, 1.0f); // 0.0 at root, 1.0 at tip

			// Organic droop using t^2 and t^3
			float tiltOffset = b.tilt * (-relY);
			float bendOffset = b.bend * (t * t) * b.height * 0.8f;
			float centerX = b.rootX + tiltOffset + bendOffset;

			// Taper: Sharp point at the tip
			float halfW = b.halfWidth * (1.0f - t * t);

			for (int px = startX; px <= endX; ++px) {
				float dist = Abs((float)px - centerX);

				// Soft edges for anti-aliasing
				float alpha = Smoothstep(halfW + 0.5f, halfW - 0.5f, dist);
				if (alpha <= 0.0f) {
					continue;
				}

				// --- ORGANIC COLORING ---

				// Root is dark (ambient occlusion), tip is bright (sunlight translucent)
				float ao = Lerp(0.2f, 1.0f, t);

				// Alive Color (Vibrant Green)
				float aliveR = Lerp(0.05f, 0.30f, t);
				float aliveG = Lerp(0.15f, 0.75f, t);
				float aliveB = Lerp(0.05f, 0.15f, t);

				// Dead Color (Dry Yellow/Brown)
				float deadR = Lerp(0.20f, 0.60f, t);
				float deadG = Lerp(0.15f, 0.50f, t);
				float deadB = Lerp(0.05f, 0.20f, t);

				// Blend based on blade's vitality
				float finalR = Lerp(deadR, aliveR, b.vitality) * b.shade * ao;
				float finalG = Lerp(deadG, aliveG, b.vitality) * b.shade * ao;
				float finalB = Lerp(deadB, aliveB, b.vitality) * b.shade * ao;

				// Fake drop shadow at the very root to anchor it to the dirt
				if (t < 0.1f)
					alpha *= (t / 0.1f);

				pixels[py * Width + px] =
					BlendColors(pixels[py * Width + px], finalR, finalG, finalB, alpha);
			}
		}
	}
	return pixels;
}

// Mandelbrot set — stress-tests texture coordinate precision & mip filtering.
template <uint32_t Width, uint32_t Height>
inline auto GenerateMandelbrot() -> std::vector<uint32_t> {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

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
template <uint32_t Width, uint32_t Height>
inline auto GenerateColorWheel() -> std::vector<uint32_t> {
	std::vector<uint32_t> pixels(static_cast<size_t>(Width * Height));

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
