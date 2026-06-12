// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <array>
#include <cstdint>
#include <vector>

namespace ZHLN::PBR {
std::vector<uint32_t> GenerateBRDFLUT(uint32_t width, uint32_t height);
std::array<JPH::Vec4, 9> GenerateDiffuseSH();
std::vector<std::vector<uint32_t>> GenerateSpecularMip(uint32_t size, float roughness);
} // namespace ZHLN::PBR
