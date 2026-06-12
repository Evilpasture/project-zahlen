// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Jolt/Jolt.h>
#include <Utils.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN::PBR {

// Standard Van der Corput radical inverse sequence
float RadicalInverse_VdC(uint32_t bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f; // / 0x100000000
}

// Hammersley sequence for low-discrepancy Monte Carlo sampling
std::pair<float, float> Hammersley(uint32_t i, uint32_t N) {
	return {float(i) / float(N), RadicalInverse_VdC(i)};
}

// GGX importance sampling
JPH::Vec3 ImportanceSampleGGX(float u1, float u2, float roughness) {
	const float a = roughness * roughness;
	const float phi = 2.0f * 3.14159265f * u1;

	// Clamp the division so it never goes negative
	float cosTheta = std::sqrt(std::max((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2), 0.0f));

	// Clamp 1 - cos^2 so it never underflows below 0.0f
	float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));

	return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

float GeometrySchlickGGX(float NdotV, float roughness) {
	float a = roughness;
	float k = (a * a) / 2.0f; // For IBL geometry, k = alpha^2 / 2
	return NdotV / (NdotV * (1.0f - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
	return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

// Generates the 2D texture bytes
std::vector<uint32_t> GenerateBRDFLUT(uint32_t width, uint32_t height) {
	std::vector<uint32_t> pixels(static_cast<size_t>(width * height));

	for (uint32_t y = 0; y < height; ++y) {
		// FIX 1: Clamp roughness to prevent divide-by-zero in Importance Sampling
		float roughness = std::max(float(y) / float(height - 1), 0.001f);

		for (uint32_t x = 0; x < width; ++x) {
			// FIX 2: Clamp NdotV so the left column doesn't evaluate to pure black 0.0
			float NdotV = std::max(float(x) / float(width - 1), 0.001f);

			float A = 0.0f;
			float B = 0.0f;

			float sqrtArg = 1.0f - NdotV * NdotV;
			JPH::Vec3 V(std::sqrt(std::max(sqrtArg, 0.0f)), 0.0f, NdotV);
			JPH::Vec3 N(0.0f, 0.0f, 1.0f);

			constexpr uint32_t SAMPLE_COUNT = 128;
			for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) {
				auto [u1, u2] = Hammersley(i, SAMPLE_COUNT);
				JPH::Vec3 H = ImportanceSampleGGX(u1, u2, roughness);
				JPH::Vec3 L = (2.0f * V.Dot(H) * H - V).Normalized();

				float NdotL = std::max(L.GetZ(), 0.0f);
				float NdotH = std::max(H.GetZ(), 0.0f);
				float VdotH = std::max(V.Dot(H), 0.0f);

				if (NdotL > 0.0f) {
					// Correct geometry constant 'k' for IBL
					float k = (roughness * roughness) / 2.0f;

					float Vis_SchlickV = NdotV * (1.0f - k) + k;
					float Vis_SchlickL = NdotL * (1.0f - k) + k;

					// FIX 3: The mathematically perfect Epic Games cancellation.
					// NdotV is algebraically cancelled out, and NdotL is preserved
					// in the numerator so the LUT isn't blown out to yellow!
					float G_Vis =
						(NdotL * VdotH) / (Vis_SchlickV * Vis_SchlickL * std::max(NdotH, 0.001f));

					float Fc = std::pow(std::max(1.0f - VdotH, 0.0f), 5.0f);

					A += (1.0f - Fc) * G_Vis;
					B += Fc * G_Vis;
				}
			}

			A /= float(SAMPLE_COUNT);
			B /= float(SAMPLE_COUNT);

			auto r = static_cast<uint8_t>(ZHLN::Clamp(A, 0.0f, 1.0f) * 255.0f);
			auto g = static_cast<uint8_t>(ZHLN::Clamp(B, 0.0f, 1.0f) * 255.0f);
			pixels[y * width + x] = 0xFF000000u | (uint32_t(g) << 8) | r;
		}
	}
	return pixels;
}
} // namespace ZHLN::PBR
