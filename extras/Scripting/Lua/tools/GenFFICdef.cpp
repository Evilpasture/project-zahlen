/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/Scripting/Lua/tools/GenFFICdef.cpp
//
// Build-time generator for the component half of ffi_cdef.
//
// Reads core's Components through ZHLN::Reflect and writes a Lua module whose
// single value is the C text LuaJIT's ffi.cdef consumes. The point is that
// nobody maintains that text: add a field to a component, or add a component,
// and the layouts the scripts see follow on the next build.
//
// Layout is derived, not assumed. ForEachFieldInfo reports each member's real
// byte offset, so padding is emitted to bridge the measured gaps and a final
// pad brings the struct to its real sizeof. That is what makes the two failure
// modes the hand-written file had -- a Jolt vector spelled as float[3], a
// uint64 handle spelled as uint32 -- impossible to reintroduce quietly: both
// show up as an offset the generator would have to disagree with itself about.
//
// Anything MapCType cannot name is emitted as a byte blob of the correct size
// and alignment rather than guessed at, so an unmapped type costs a script the
// use of that field, never the correctness of the ones around it.
//
// Usage: GenFFICdef <output.lua>

#include <Scripting/Lua/tools/FFICTypeMap.hpp>

#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Reflection.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace {

    std::string out;
    int         structsEmitted = 0;
    int         fieldsEmitted  = 0;
    int         opaqueFields   = 0;
    int         padFields      = 0;

    /// The bare identifier, without namespace or enclosing-class qualification.
    auto BareName(std::string_view qualified) -> std::string {
        const auto pos = qualified.rfind("::");
        return std::string(pos == std::string_view::npos ? qualified : qualified.substr(pos + 2));
    }

    void EmitAuxiliaryTypes() {
        // Types the components are built from that are not Components members.
        // Their layouts are fixed by the C++ definitions named in the comment.
        out += "      typedef struct ZHLN_Entity {\n"
               "          uint32_t index;\n"
               "          uint32_t generation;\n"
               "      } ZHLN_Entity;\n\n";

        out += "      typedef struct ZHLN_FixedString64 {\n"
               "          char   data[64];\n"
               "          size_t len;\n"
               "      } ZHLN_FixedString64;\n\n";
        out += "      typedef struct ZHLN_FixedString128 {\n"
               "          char   data[128];\n"
               "          size_t len;\n"
               "      } ZHLN_FixedString128;\n\n";
        out += "      typedef struct ZHLN_FixedString256 {\n"
               "          char   data[256];\n"
               "          size_t len;\n"
               "      } ZHLN_FixedString256;\n\n";
    }

    /// The typedef name a FixedString field refers to, chosen by capacity.
    auto FixedStringTypedef(std::size_t cppSize) -> std::string {
        // FixedString<N> is N bytes of storage plus a size_t length.
        if (cppSize == 64 + sizeof(std::size_t)) return "ZHLN_FixedString64";
        if (cppSize == 128 + sizeof(std::size_t)) return "ZHLN_FixedString128";
        if (cppSize == 256 + sizeof(std::size_t)) return "ZHLN_FixedString256";
        return {};
    }

    template <typename Comp>
    void EmitStruct() {
        const std::string name = BareName(ZHLN::Reflect::TypeName<Comp>());

        out += "      typedef struct " + name + " {\n";

        std::size_t cursor   = 0;
        int         padIndex = 0;

        ZHLN::Reflect::ForEachFieldInfo<Comp>(
            [&]<typename Field>(std::string_view fieldName, std::size_t offset) {
                // Bridge the gap the compiler left before this member. Deriving
                // it from the measured offset is what keeps alignment-sensitive
                // members (Jolt's 16-byte vectors) from shifting their
                // successors.
                if (offset > cursor) {
                    out += "          char _pad" + std::to_string(padIndex++) + "[" +
                           std::to_string(offset - cursor) + "];\n";
                    ++padFields;
                }

                constexpr auto decl = ZHLN::FFI::MapCType<Field>();
                const std::string field(fieldName);

                if (decl.isOpaque()) {
                    // No C counterpart. Occupy exactly the right bytes at
                    // exactly the right alignment and say why, so the gap is
                    // visible in the generated file instead of silent.
                    out += "          char " + field + "[" + std::to_string(decl.size) +
                           "] __attribute__((aligned(" + std::to_string(decl.align) +
                           "))); // no C layout\n";
                    ++opaqueFields;
                } else {
                    std::string base = decl.base;
                    if (base == "ZHLN_FixedString") {
                        base = FixedStringTypedef(decl.size);
                        if (base.empty()) {
                            // Unknown capacity: fall back to bytes rather than
                            // emitting a typedef that does not exist.
                            out += "          char " + field + "[" + std::to_string(decl.size) +
                                   "] __attribute__((aligned(" + std::to_string(decl.align) +
                                   "))); // FixedString capacity has no typedef\n";
                            ++opaqueFields;
                            cursor = offset + decl.size;
                            return;
                        }
                    }

                    out += "          " + base + " " + field;
                    if (decl.count > 1) {
                        out += "[" + std::to_string(decl.count) + "]";
                    }
                    if (decl.aligned16) {
                        // Plain float[4] would only promise 4-byte alignment and
                        // every later member would move.
                        out += " __attribute__((aligned(16)))";
                    }
                    out += ";\n";
                    ++fieldsEmitted;
                }

                cursor = offset + decl.size;
            });

        // Trailing padding, so the generated struct is the same size as the C++
        // one and arrays of it stride identically.
        if (sizeof(Comp) > cursor) {
            out += "          char _padEnd[" + std::to_string(sizeof(Comp) - cursor) + "];\n";
        }

        out += "      } " + name + ";\n\n";
        ++structsEmitted;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 2) {
        std::fprintf(stderr, "usage: GenFFICdef <output.lua>\n");
        return 2;
    }

    out += "-- Generated by extras/Scripting/Lua/tools/GenFFICdef.cpp. Do not edit.\n"
           "--\n"
           "-- Component layouts read from ZHLN::Components through ZHLN::Reflect.\n"
           "-- Regenerate by rebuilding; ffi_cdef.fnl loads this as C text.\n"
           "return [[\n";

    EmitAuxiliaryTypes();
    ZHLN::Reflect::ForEachNestedType<ZHLN::Components>([&]<typename Comp>() { EmitStruct<Comp>(); });

    out += "]]\n";

    std::ofstream file(argv[1], std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "error: cannot write %s\n", argv[1]);
        return 1;
    }
    file << out;
    if (!file) {
        std::fprintf(stderr, "error: write failed for %s\n", argv[1]);
        return 1;
    }

    std::printf("GenFFICdef: %s -- %d structs, %d fields, %d padding, %d opaque\n", argv[1],
                structsEmitted, fieldsEmitted, padFields, opaqueFields);

    if (structsEmitted == 0) {
        std::fprintf(stderr,
                     "error: reflected no components. Either Components is empty or this "
                     "compiler has no static reflection, in which case the generated layouts "
                     "would be wrong rather than merely missing.\n");
        return 1;
    }
    return 0;
}
