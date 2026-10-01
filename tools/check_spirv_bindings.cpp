// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Compile-only regression for the constexpr SPIR-V catalog reader:
//   g++ -std=c++23 -Iinclude -fsyntax-only tools/check_spirv_bindings.cpp
// With GCC 12, also pass '-D__has_feature(x)=0' for Description.hpp.
// This declaration stream exercises direct, fixed-array, nested-array and
// runtime-array samplers, plus an image array that must stay a resource.

#include "../src/vulkan/pipeline/SpirvBindings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr auto Op(uint16_t opcode, uint16_t words) -> uint32_t {
    return (static_cast<uint32_t>(words) << 16) | opcode;
}

template <size_t N>
consteval auto AsBytes(const std::array<uint32_t, N>& words) -> std::array<uint8_t, N * 4> {
    std::array<uint8_t, N * 4> bytes {};
    for (size_t i = 0; i < N; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            bytes[i * 4 + j] = static_cast<uint8_t>(words[i] >> (j * 8));
        }
    }
    return bytes;
}

// The reader only needs the declarations; OpEntryPoint is enough to identify
// the stage and the decorated variables, without a function body or shader.
constexpr auto kWords = std::to_array<uint32_t>({
    0x07230203, 0x00010000, 0, 32, 0, // SPIR-V header
    Op(15, 5), 4, 1, 0x6e69616d, 0, // OpEntryPoint Fragment %1 "main"
    Op(5, 3), 10, 'd', // OpName %10 "d" (direct sampler)
    Op(5, 3), 11, 'f', // fixed sampler array
    Op(5, 3), 12, 'n', // nested sampler array in set 1
    Op(5, 3), 13, 'r', // runtime sampler array
    Op(5, 3), 14, 'i', // fixed image array
    Op(71, 4), 10, 33, 0,
    Op(71, 4), 11, 33, 1,
    Op(71, 4), 12, 33, 2,
    Op(71, 4), 12, 34, 1,
    Op(71, 4), 13, 33, 3,
    Op(71, 4), 14, 33, 4,
    Op(26, 2), 2, // OpTypeSampler %2
    Op(22, 3), 4, 32, // OpTypeFloat %4 32
    Op(25, 9), 3, 4, 1, 0, 0, 0, 1, 0, // OpTypeImage %3 %4 2D
    Op(21, 4), 31, 32, 0, // OpTypeInt %31 32 0
    Op(43, 4), 31, 30, 9, // OpConstant %31 %30 9
    Op(28, 4), 5, 2, 30, // OpTypeArray %5 %2 %30
    Op(28, 4), 6, 5, 30, // OpTypeArray %6 %5 %30
    Op(29, 3), 7, 2, // OpTypeRuntimeArray %7 %2
    Op(28, 4), 8, 3, 30, // OpTypeArray %8 %3 %30
    Op(32, 4), 9, 0, 2, // OpTypePointer %9 UniformConstant %2
    Op(32, 4), 15, 0, 5,
    Op(32, 4), 16, 0, 6,
    Op(32, 4), 17, 0, 7,
    Op(32, 4), 18, 0, 8,
    Op(59, 4), 9, 10, 0, // OpVariable %9 %10 UniformConstant
    Op(59, 4), 15, 11, 0,
    Op(59, 4), 16, 12, 0,
    Op(59, 4), 17, 13, 0,
    Op(59, 4), 18, 14, 0,
});
constexpr auto kBytes = AsBytes(kWords);
constexpr auto kSet0 = ZHLN::Vk::SpirvBindings::Parse(kBytes, 0);
constexpr auto kSet1 = ZHLN::Vk::SpirvBindings::Parse(kBytes, 1);

static_assert(kSet0.Complete() && kSet0.EntryPointCount() == 1 && kSet0.IsEntryPoint("main") && kSet0.ExecutionModel() == 4);
static_assert(kSet0.Count() == 4 && kSet0.HighestDeclaredSet() == 1);
static_assert(kSet0.DeclaresSampler("d") && kSet0.DeclaresSampler("f") && kSet0.DeclaresSampler("r"));
static_assert(!kSet0.DeclaresResource("f") && !kSet0.DeclaresResource("r") && kSet0.DeclaresResource("i"));
static_assert(kSet1.Complete() && kSet1.Count() == 1 && kSet1.DeclaresSampler("n") && !kSet0.DeclaresSampler("n"));

} // namespace
