/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Standalone check of the C++ -> C type mapping in FFICTypeMap.hpp.
//
// The mapper is ordinary template code, so unlike the reflection driver that
// uses it, this compiles and runs on a compiler with no static-reflection
// support. It walks the field types Components.hpp actually uses and asserts
// that every spelling the mapper produces occupies exactly the bytes and
// demands exactly the alignment of the C++ type it stands for -- the property
// the generated layouts depend on.
//
// Not part of the shipped build; run it directly:
//   clang++ -std=c++26 -Iinclude -Iextras -Iextern/JoltPhysics \
//       -DJPH_DOUBLE_PRECISION extras/scripting_lua/tools/test_c_type_map.cpp

#include <scripting_lua/tools/FFICTypeMap.hpp>

#include <Zahlen/Components.hpp>
#include <Zahlen/Types.hpp>

#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace {

    // What the generated C actually occupies for a given spelling.
    struct CEntity {
        uint32_t index;
        uint32_t generation;
    };
    struct CFS64 {
        char     data[64];
        std::size_t len;
    };
    struct CFS128 {
        char     data[128];
        std::size_t len;
    };
    struct CFS256 {
        char     data[256];
        std::size_t len;
    };

    constexpr std::size_t ScalarSize(const char* base) {
        if (base == nullptr) return 0;
        const std::string_view n = base;
        if (n == "bool" || n == "int8_t" || n == "uint8_t" || n == "char") return 1;
        if (n == "int16_t" || n == "uint16_t") return 2;
        if (n == "float" || n == "int32_t" || n == "uint32_t") return 4;
        if (n == "double" || n == "int64_t" || n == "uint64_t") return 8;
        return 0;
    }

    constexpr std::size_t ScalarAlign(const char* base) { return ScalarSize(base); }

    int failures = 0;

    template <typename T>
    void Check(const char* label) {
        constexpr auto decl = ZHLN::FFI::MapCType<T>();

        if (decl.isOpaque()) {
            // Opaque is legitimate, but only for types we know have no C
            // layout. Report it so the list stays deliberate rather than
            // growing silently because a mapping was forgotten.
            printf("  opaque   %-34s size=%3zu align=%2zu\n", label, decl.size, decl.align);
            return;
        }

        std::size_t cSize  = 0;
        std::size_t cAlign = 0;
        const std::string_view base = decl.base;

        if (base == "ZHLN_Entity") {
            cSize  = sizeof(CEntity) * decl.count;
            cAlign = alignof(CEntity);
        } else if (base == "ZHLN_FixedString") {
            // The driver picks the per-capacity typedef; match by C++ size.
            cSize  = decl.size;
            cAlign = 8;
            const bool knownCapacity = decl.size == sizeof(CFS64) || decl.size == sizeof(CFS128) ||
                                       decl.size == sizeof(CFS256);
            if (!knownCapacity) {
                printf("  FAIL     %-34s FixedString capacity %zu has no typedef\n", label, decl.size);
                ++failures;
                return;
            }
        } else {
            cSize  = ScalarSize(decl.base) * decl.count;
            cAlign = decl.aligned16 ? 16 : ScalarAlign(decl.base);
        }

        const bool ok = cSize == sizeof(T) && cAlign == alignof(T);
        if (!ok) {
            printf("  FAIL     %-34s C=%s[%d] size=%zu align=%zu  C++ size=%zu align=%zu\n",
                   label, decl.base, decl.count, cSize, cAlign, sizeof(T), alignof(T));
            ++failures;
        } else {
            printf("  ok       %-34s %s[%d] size=%zu align=%zu\n",
                   label, decl.base, decl.count, cSize, cAlign);
        }
    }

} // namespace

int main() {
    using namespace ZHLN;

    printf("scalars\n");
    Check<float>("float");
    Check<bool>("bool");
    Check<uint8_t>("uint8_t");
    Check<int32_t>("int32_t");
    Check<uint32_t>("uint32_t");
    Check<uint64_t>("uint64_t");
    Check<int>("int");

    printf("jolt math (measured 16-byte aligned)\n");
    Check<JPH::Vec3>("JPH::Vec3");
    Check<JPH::Vec4>("JPH::Vec4");
    Check<JPH::Quat>("JPH::Quat");
    Check<JPH::Mat44>("JPH::Mat44");

    printf("handles and enums\n");
    Check<TextureHandle>("TextureHandle");
    Check<AssetID>("AssetID");
    Check<MaterialID>("MaterialID");
    Check<Entity>("Entity");
    Check<TextAlignment>("TextAlignment");
    Check<LightType>("LightType");

    printf("fixed strings\n");
    Check<String64>("String64");
    Check<String128>("String128");
    Check<String256>("String256");

    printf("arrays\n");
    Check<std::array<float, 4>>("std::array<float,4>");
    Check<std::array<Entity, 8>>("std::array<Entity,8>");

    printf("types with no C layout\n");
    Check<Components::UIChildCacheComponent>("UIChildCacheComponent");

    printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
