#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <cstdint>
#include <vector>

namespace ZHLN::PBR {
std::vector<uint32_t> GenerateBRDFLUT(uint32_t width, uint32_t height);
std::vector<std::vector<uint32_t>> GenerateIrradianceCubemap();
std::vector<std::vector<uint32_t>> GenerateSpecularMip(uint32_t size, float roughness);
} // namespace ZHLN::PBR
