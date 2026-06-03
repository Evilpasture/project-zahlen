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
	float a = roughness * roughness;
	float phi = 2.0f * 3.14159265f * u1;
	float cosTheta = std::sqrt((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2));
	float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
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
		float roughness = float(y) / float(height - 1);
		for (uint32_t x = 0; x < width; ++x) {
			float NdotV = float(x) / float(width - 1);

			float A = 0.0f;
			float B = 0.0f;

			JPH::Vec3 V(std::sqrt(1.0f - NdotV * NdotV), 0.0f, NdotV);
			JPH::Vec3 N(0.0f, 0.0f, 1.0f);

			const uint32_t SAMPLE_COUNT = 128;
			for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) {
				auto [u1, u2] = Hammersley(i, SAMPLE_COUNT);
				JPH::Vec3 H = ImportanceSampleGGX(u1, u2, roughness);
				JPH::Vec3 L = (2.0f * V.Dot(H) * H - V).Normalized();

				float NdotL = std::max(L.GetZ(), 0.0f);
				float NdotH = std::max(H.GetZ(), 0.0f);
				float VdotH = std::max(V.Dot(H), 0.0f);

				if (NdotL > 0.0f) {
					float G = GeometrySmith(NdotV, NdotL, roughness);
					float G_Vis = (G * VdotH) / (NdotH * NdotV);
					float Fc = std::pow(1.0f - VdotH, 5.0f);

					A += (1.0f - Fc) * G_Vis;
					B += Fc * G_Vis;
				}
			}

			A /= float(SAMPLE_COUNT);
			B /= float(SAMPLE_COUNT);

			// Pack values into R8G8B8A8 (A maps to Red, B maps to Green)
			auto r = static_cast<uint8_t>(ZHLN::Clamp(A, 0.0f, 1.0f) * 255.0f);
			auto g = static_cast<uint8_t>(ZHLN::Clamp(B, 0.0f, 1.0f) * 255.0f);

			pixels[y * width + x] = 0xFF000000u | (uint32_t(g) << 8) | r;
		}
	}
	return pixels;
}

} // namespace ZHLN::PBR
