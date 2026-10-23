// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tools/zshader/GpuTypes.cpp
//
// Slang owns the GPU memory layout; this is the half that makes C++ listen.
// It compiles the gpu_abi module in-process through CompileSlangEntry -- the
// compile it shares with the catalog's reflection (SlangReflect.cpp) -- and
// walks the reflected layout: member
// offsets, sizes, strides and alignments come from the compiler's own model.
// The Slang type decides the C++ spelling by itself -- float2/float3/float4
// to JPH::Float2/Float3/Float4, uint2/int2 to std::array lanes, float4x4 to
// JPH::Mat44, a fixed array to std::array of its element, and enums to generated types.
// There is no table here restating either half, so a Slang edit carries its
// spelling with it and nothing can skew a layout behind the shader's back.
//
// Every struct is emitted here, including the five the engine and the shaders
// both have a word for -- Particle, ParticleEmitterParams,
// MeshParticleEmitterParams, Light and FrameUniforms. There is no hand-written
// half: the emitted definition *is* the ABI, and it is complete on its own --
// every gap between members is a named _padN run and every lane carries the
// alignment Slang gave it, so the struct's extent and its members' offsets are
// spelled out in the text rather than inferred by the host compiler. That is
// why nothing below re-checks the emission with static_asserts: there is no
// pack the host compiler could choose that the text has not already fixed, and
// a check that cannot fail is ceremony, not safety. So a Slang edit cannot
// reshape a type the engine names, because the engine no longer names one: it
// hands the renderer ParticleEmitterDesc, LightDesc and FrameData, and
// src/render/GpuPack.cpp writes the generated structs field by field.
//
// The scene push blob is emitted from the same walk: kScenePushLayout, the
// frame-address offsets and the descriptor-index word inside
// DescriptorHeapPushData, is what src/render/GpuAbi.cpp exports to the heap
// writer. One compile, one output (the header); the module's own SPIR-V is
// still written --out-abi-spv as the proof that the ABI module codegens for
// compute, but nothing embeds it any more. There is no second reader of the
// bytes to hold the header against, and none is wanted: the header is the ABI.
//
// One thing the walk refuses to derive, and fails on instead of guessing:
//
//   * Struct footprints are recomputed as AlignUp(extent, maxAlign): layout
//     reports ClusterVolume as 16 bytes (ConstantBuffer rounding) while its
//     StructuredBuffer stride -- the number the engine uploads by -- is 8.
//     The walk trusts Slang for member offsets and recomputes the rest.

#include "ZShader.hpp"

#include "SlangReflect.hpp" // CompileSlangEntry: the compile this mode shares with the catalog

#include <slang-com-helper.h> // Slang SDK utilities, next to slang.h
#include <slang-com-ptr.h>    // Slang::ComPtr
#include <slang.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::ZShader {
namespace {

// The contract with gpu_abi.slang: the global that carries the wrapper, the
// wrapper's type, and the entry point that anchors the link. Literals, not
// flags: a gpu_abi that renames any of these is a different module, and the
// walk fails naming what it cannot find.
constexpr std::string_view kAbiGlobal  = "abi";
constexpr std::string_view kAbiWrapper = "GpuAbiTypes";
constexpr std::string_view kEntryPoint = "CSMain";
constexpr std::string_view kSceneGlobal = "scene";
constexpr std::string_view kHeapStruct  = "DescriptorHeapPushData";

// GPU enums are declared with an explicit uint underlying type in Slang. The
// walk accepts their reflected four-byte layouts and uint32_t-valued cases only;
// it does not translate or rename case lists by hand.
// The storage spellings: Jolt's pod lane types, one per Slang float vector
// width. JPH::Vec3/Vec4 are the *compute* types (16 bytes, 16-aligned, three
// live lanes in a Vec3) and are never a storage destination.
constexpr std::string_view kFloat2 = "::JPH::Float2";
constexpr std::string_view kFloat3 = "::JPH::Float3";
constexpr std::string_view kFloat4 = "::JPH::Float4";
constexpr std::string_view kMat4   = "::JPH::Mat44";

constexpr auto kUniform = SLANG_PARAMETER_CATEGORY_UNIFORM;

// `value` rounded up to the next multiple of `alignment`: what a host ABI
// means by "the size a struct has once its last member is padded out".
auto AlignUp(uint32_t value, uint32_t alignment) -> uint32_t {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// Slang diagnostics as text, empty when there are none: every API step below
// carries its reason in the blob, and the ABI modules compile warning-free,
// so any output is either the cause of a failure or a failure itself.
auto DiagText(slang::IBlob* diagnostics) -> std::string {
    if (diagnostics == nullptr) {
        return {};
    }
    const char* text = static_cast<const char*>(diagnostics->getBufferPointer());
    return text != nullptr ? std::string {text} : std::string {};
}

auto CheckDiagnostics(std::string_view module, std::string_view stage, slang::IBlob* diagnostics) -> void {
    const std::string text = DiagText(diagnostics);
    if (text.empty()) {
        return;
    }
    Fail("gpu types: Slang reported diagnostics {} '{}'; the ABI modules compile warning-free:\n{}", stage, module, text);
}

// A Slang reflection kind as a word: the Fail messages below name what the
// tool found, not a flag bit it cannot pronounce.
auto KindName(slang::TypeReflection::Kind kind) -> std::string_view {
    using Kind = slang::TypeReflection::Kind;
    if (kind == Kind::Scalar) {
        return "scalar";
    }
    if (kind == Kind::Vector) {
        return "vector";
    }
    if (kind == Kind::Matrix) {
        return "matrix";
    }
    if (kind == Kind::Array) {
        return "array";
    }
    if (kind == Kind::Struct) {
        return "struct";
    }
    if (kind == Kind::Enum) {
        return "enum";
    }
    return "resource";
}

// How one Slang member is spelled in C++: data, chosen by the member's kind
// alone, rendered to text once at emission. Never string fragments joined
// through the pipeline: Array nests structurally and Render is the only
// function that turns a type into text.
enum class CxxForm { Scalar, Enum, Vec, Mat, Array, Struct };

struct CxxType {
    CxxForm                  form = CxxForm::Scalar;
    std::string              spelling {}; // Scalar/Enum/Vec/Mat/Struct; Array recurses instead
    std::unique_ptr<CxxType> element {}; // Array element
    uint32_t                 count = 0; // Array length
    uint32_t                 size  = 0;
    uint32_t                 cxxAlign = 0;
};

// A rendered spelling: the base text plus any C-array dimensions -- always
// empty now, since every repeat is a std::array. Kept as the single shape
// Render returns rather than a bare string, so a future repeat spelling has
// one place to land.
struct Spelling {
    std::string           base {};
    std::vector<uint32_t> dims {};
};

auto Render(const CxxType& type, std::string_view structName, std::string_view memberName) -> Spelling {
    if (type.form == CxxForm::Array) {
        Spelling inner = Render(*type.element, structName, memberName);
        if (!inner.dims.empty()) {
            Fail("gpu types: {}.{} mixes array spellings; one member renders one way", structName, memberName);
        }
        return {.base = std::format("::std::array<{}, {}>", inner.base, type.count)};
    }
    return {.base = type.spelling};
}

// One emitted member: a Slang field or a synthesized padding run. Padding
// carries no annotation -- it is a byte array sized to the gap the reflection
// left -- which is what distinguishes it from Slang's own _paddingCenter and
// _pad members (real fields, spelled like any other).
struct Field {
    std::string name {};
    CxxType     type {};
    uint32_t    offset     = 0;
    uint32_t    size       = 0;
    uint32_t    slangAlign = 0;
    uint32_t    cxxAlign   = 0;
    bool        isPad      = false;
};

struct StructDef {
    std::string        name {};
    uint32_t           size  = 0;
    uint32_t           align = 0;
    std::vector<Field> fields {};
};

struct StructBuildResult {
    StructDef              definition;
    std::vector<StructDef> nested;
};

struct EnumValue {
    std::string name {};
    uint32_t    value = 0;
};

struct EnumDef {
    std::string          name {};
    std::vector<EnumValue> values {};
};

// The member's alignment as Slang seats it: every alignas in the header
// answers to this number, so a non-positive one fails rather than emitting a
// struct whose lanes the host compiler would place somewhere else.
auto ReadAlignment(std::string_view structName, std::string_view memberName, slang::TypeLayoutReflection* layout) -> uint32_t {
    const int32_t align = layout->getAlignment(kUniform);
    if (align <= 0) {
        Fail("gpu types: {}.{} reports an alignment of {}; every member seats on a positive boundary", structName, memberName, align);
    }
    return static_cast<uint32_t>(align);
}

// Reflects one Slang enum case list into a type-safe C++ enum. The GPU ABI
// currently uses only four-byte uint enums; rejecting other layouts here keeps
// the generated type and the shader's storage representation explicit.
auto ReflectEnum(
    std::string_view structName,
    std::string_view memberName,
    slang::TypeReflection* type,
    slang::TypeLayoutReflection* layout
) -> EnumDef {
    const char*            rawName = type->getName();
    if (rawName == nullptr || rawName[0] == '\0') {
        Fail("gpu types: {}.{} has an unnamed enum type", structName, memberName);
    }
    const std::string_view enumName = rawName;
    if (!IsIdentifier(enumName)) {
        Fail("gpu types: {}.{} enum '{}' is not a C++ identifier", structName, memberName, enumName);
    }

    const size_t  size  = layout->getSize(kUniform);
    const int32_t align = layout->getAlignment(kUniform);
    if (size != sizeof(uint32_t) || align != alignof(uint32_t)) {
        Fail(
            "gpu types: {}.{} enum '{}' occupies {} bytes with alignment {}; generated enums require uint32_t storage",
            structName,
            memberName,
            enumName,
            size,
            align
        );
    }
    if (type->getFieldCount() == 0) {
        Fail("gpu types: {}.{} enum '{}' has no cases", structName, memberName, enumName);
    }

    EnumDef result {.name = std::string {enumName}};
    result.values.reserve(type->getFieldCount());
    for (unsigned i: std::views::iota(0u, type->getFieldCount())) {
        slang::VariableReflection* value = type->getFieldByIndex(i);
        const char*               name  = value->getName();
        if (name == nullptr || name[0] == '\0' || !IsIdentifier(name)) {
            Fail("gpu types: {}.{} enum '{}' has a case that is not a C++ identifier", structName, memberName, enumName);
        }
        int64_t integer = 0;
        if (SLANG_FAILED(value->getDefaultValueInt(&integer))) {
            Fail("gpu types: {}.{} enum '{}.{}' has no reflected integer value", structName, memberName, enumName, name);
        }
        if (integer < 0 || static_cast<uint64_t>(integer) > std::numeric_limits<uint32_t>::max()) {
            Fail("gpu types: {}.{} enum '{}.{}' value {} does not fit uint32_t", structName, memberName, enumName, name, integer);
        }
        result.values.push_back(EnumValue {.name = name, .value = static_cast<uint32_t>(integer)});
    }
    return result;
}

auto SameEnum(const EnumDef& lhs, const EnumDef& rhs) -> bool {
    if (lhs.name != rhs.name || lhs.values.size() != rhs.values.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.values.size(); ++i) {
        if (lhs.values[i].name != rhs.values[i].name || lhs.values[i].value != rhs.values[i].value) {
            return false;
        }
    }
    return true;
}

// The Slang-kind-to-C++ mapping for one member: float vectors are their Jolt
// pod lane type, 32-bit integer vectors are std::array lanes, a float4x4 is
// JPH::Mat44, scalar kinds map to stdint, and a Slang enum gets a generated
// enum. Fixed arrays map to std::array when their reflected stride is tight.
auto MapMember(
    std::string_view structName,
    std::string_view memberName,
    slang::TypeReflection* type,
    slang::TypeLayoutReflection* layout,
    std::vector<EnumDef>& enums
) -> CxxType {
    using Kind = slang::TypeReflection::Kind;
    const Kind             kind = type->getKind();
    const auto             scalar = type->getScalarType();
    const bool             isFloat32 = scalar == slang::TypeReflection::ScalarType::Float32;

    if (kind == Kind::Enum) {
        EnumDef definition = ReflectEnum(structName, memberName, type, layout);
        const auto existing = std::ranges::find_if(enums, [&definition](const EnumDef& candidate) { return candidate.name == definition.name; });
        if (existing == enums.end()) {
            enums.push_back(std::move(definition));
        } else if (!SameEnum(*existing, definition)) {
            Fail("gpu types: enum '{}' is reflected with conflicting case lists", definition.name);
        }
        return {
            .form = CxxForm::Enum,
            .spelling = std::format("::ZHLN::Slang::{}", type->getName()),
            .size = sizeof(uint32_t),
            .cxxAlign = alignof(uint32_t),
        };
    }
    if (kind == Kind::Struct) {
        // Named by the caller: the member's own type, resolved through the
        // nested walk rather than any registry.
        return {.form = CxxForm::Struct};
    }
    if (kind == Kind::Matrix) {
        const uint32_t rows = static_cast<uint32_t>(type->getRowCount());
        const uint32_t cols = static_cast<uint32_t>(type->getColumnCount());
        if (rows != 4 || cols != 4 || !isFloat32) {
            Fail("gpu types: {}.{} is not a float4x4; only float4x4 matrices map to JPH::Mat44", structName, memberName);
        }
        if (layout->getMatrixLayoutMode() != SLANG_MATRIX_LAYOUT_COLUMN_MAJOR) {
            Fail("gpu types: {}.{} is not column-major; the host Mat44 assumes column-major", structName, memberName);
        }
        return {.form = CxxForm::Mat, .spelling = std::string {kMat4}, .size = 64, .cxxAlign = 16};
    }
    if (kind == Kind::Array) {
        slang::TypeReflection*      elementType   = type->getElementType();
        slang::TypeLayoutReflection* elementLayout = layout->getElementTypeLayout();
        const Kind                  elementKind   = elementType->getKind();
        if (elementKind == Kind::Array) {
            Fail("gpu types: {}.{} is multi-dimensional; only one-dimensional fixed arrays map", structName, memberName);
        }
        if (elementKind == Kind::Struct) {
            Fail("gpu types: {}.{} is an array of structs, which has no C++ spelling here", structName, memberName);
        }
        if (elementKind == Kind::Scalar) {
            Fail("gpu types: {}.{} is an array of scalars; std140 strides scalar lanes, which no C++ array spells", structName, memberName);
        }
        if (elementKind != Kind::Vector && elementKind != Kind::Matrix) {
            Fail("gpu types: {}.{} is an array of {}, which has no C++ spelling here", structName, memberName, KindName(elementKind));
        }
        const uint32_t count  = static_cast<uint32_t>(type->getElementCount());
        const uint32_t stride = static_cast<uint32_t>(layout->getElementStride(kUniform));
        if (count == 0) {
            Fail("gpu types: {}.{} is unsized; only fixed arrays map", structName, memberName);
        }
        // The element maps by the same table, so a repeat of a float4 is a
        // std::array of the pod lane type -- no second spelling for repeats.
        CxxType element = MapMember(structName, memberName, elementType, elementLayout, enums);
        if (stride != element.size) {
            Fail(
                "gpu types: {}.{} strides {} but its element occupies {}; padded elements have no C++ spelling here",
                structName,
                memberName,
                stride,
                element.size
            );
        }
        const uint32_t footprint    = stride * count;
        const uint32_t elementAlign = element.cxxAlign;
        auto           boxed        = std::make_unique<CxxType>(std::move(element));
        return {.form = CxxForm::Array, .element = std::move(boxed), .count = count, .size = footprint, .cxxAlign = elementAlign};
    }
    if (kind == Kind::Vector) {
        using Scalar = slang::TypeReflection::ScalarType;
        const uint32_t components = static_cast<uint32_t>(type->getElementCount());
        if (components < 2 || components > 4) {
            Fail("gpu types: {}.{} has {} vector components; only 2, 3 and 4 map", structName, memberName, components);
        }
        if (isFloat32) {
            // The pod lane type: same lanes as the shader, layout 4-byte
            // aligned and 4/8/12/16 bytes wide. Slang's reflected alignment
            // supplies any wider vector boundary with alignas in the header.
            const std::string_view lanes = components == 2 ? kFloat2 : (components == 3 ? kFloat3 : kFloat4);
            return {.form = CxxForm::Vec, .spelling = std::string {lanes}, .size = 4 * components, .cxxAlign = 4};
        }
        if (scalar == Scalar::UInt32 || scalar == Scalar::Int32) {
            // std::array preserves the lane count and footprint; the Slang
            // alignment (often 8 for uint2/int2) is emitted separately.
            const std::string_view lane = scalar == Scalar::UInt32 ? "uint32_t" : "int32_t";
            return {
                .form = CxxForm::Vec,
                .spelling = std::format("::std::array<{}, {}>", lane, components),
                .size = 4 * components,
                .cxxAlign = 4,
            };
        }
        Fail("gpu types: {}.{} has a vector scalar type with no C++ spelling here", structName, memberName);
    }
    if (kind == Kind::Scalar) {
        using Scalar = slang::TypeReflection::ScalarType;
        if (scalar == Scalar::Float32) {
            return {.form = CxxForm::Scalar, .spelling = "float", .size = 4, .cxxAlign = 4};
        }
        if (scalar == Scalar::Int32) {
            return {.form = CxxForm::Scalar, .spelling = "int32_t", .size = 4, .cxxAlign = 4};
        }
        if (scalar == Scalar::UInt32) {
            return {.form = CxxForm::Scalar, .spelling = "uint32_t", .size = 4, .cxxAlign = 4};
        }
        if (scalar == Scalar::Int64) {
            return {.form = CxxForm::Scalar, .spelling = "int64_t", .size = 8, .cxxAlign = 8};
        }
        if (scalar == Scalar::UInt64) {
            return {.form = CxxForm::Scalar, .spelling = "uint64_t", .size = 8, .cxxAlign = 8};
        }
        if (scalar == Scalar::Bool) {
            Fail("gpu types: {}.{} is a bool, which has no agreed host width", structName, memberName);
        }
        Fail("gpu types: {}.{} has no C++ spelling here", structName, memberName);
    }
    Fail("gpu types: {}.{} is a {}, which has no C++ spelling here", structName, memberName, KindName(kind));
}

// Builds one struct definition from one reflected struct layout: every field
// in offset order with explicit padding between them, sized by rounding the
// member extent up to the widest member alignment. Nested structs recurse --
// Slang rejects infinitely-sized types at compile time, so the walk
// terminates by construction.
auto BuildStruct(std::string_view structName, slang::TypeLayoutReflection* layout, std::vector<EnumDef>& enums) -> StructBuildResult {
    std::vector<slang::VariableLayoutReflection*> members;
    for (unsigned i: std::views::iota(0u, layout->getFieldCount())) {
        members.push_back(layout->getFieldByIndex(i));
    }
    std::ranges::sort(members, {}, [](slang::VariableLayoutReflection* member) { return member->getOffset(kUniform); });

    StructBuildResult result {.definition = StructDef {.name = std::string {structName}}};
    StructDef& def = result.definition;
    uint32_t  cursor   = 0;
    uint32_t  padIndex = 0;
    for (slang::VariableLayoutReflection* member: members) {
        const char* name = member->getName();
        if (name == nullptr || name[0] == '\0') {
            Fail(
                "gpu types: {}.<unnamed> at offset {} has no name; a field has nothing to be checked by",
                structName,
                member->getOffset(kUniform)
            );
        }
        const std::string_view memberName = name;
        const uint32_t           offset      = static_cast<uint32_t>(member->getOffset(kUniform));
        if (offset < cursor) {
            Fail("gpu types: {}.{} at offset {} overlaps the previous member ending at {}", structName, memberName, offset, cursor);
        }
        if (offset > cursor) {
            const uint32_t gap = offset - cursor;
            def.fields.push_back(Field {
                .name       = std::format("_pad{}", padIndex++),
                .offset     = cursor,
                .size       = gap,
                .slangAlign = 1,
                .cxxAlign   = 1,
                .isPad      = true,
            });
            cursor = offset;
        }

        slang::TypeLayoutReflection* memberLayout = member->getTypeLayout();
        // Keep the declared type separate from its layout: Slang can lower an
        // enum field to its underlying scalar for storage, while VariableReflection
        // retains the enum kind and its named cases.
        slang::VariableReflection* variable = member->getVariable();
        if (variable == nullptr) {
            Fail("gpu types: {}.{} has no reflected variable", structName, memberName);
        }
        slang::TypeReflection* memberType = variable->getType();
        if (memberType == nullptr) {
            Fail("gpu types: {}.{} has no reflected variable type", structName, memberName);
        }
        const uint32_t         slangAlign = ReadAlignment(structName, memberName, memberLayout);
        CxxType                mapped     = MapMember(structName, memberName, memberType, memberLayout, enums);

        // A nested struct recurses through the same builder; anything else
        // takes its mapped spelling, held against the member size Slang
        // reported. The nested footprint is the recomputed one (AlignUp,
        // never the padded getSize).
        if (mapped.form == CxxForm::Struct) {
            const char* nestedName = memberLayout->getName();
            if (nestedName == nullptr || nestedName[0] == '\0') {
                Fail("gpu types: {}.{} is an unnamed struct; a nested type has nothing to be checked by", structName, memberName);
            }
            StructBuildResult inner = BuildStruct(nestedName, memberLayout, enums);
            mapped.spelling = inner.definition.name;
            mapped.size     = inner.definition.size;
            mapped.cxxAlign = inner.definition.align;
            std::ranges::move(inner.nested, std::back_inserter(result.nested));
            result.nested.push_back(std::move(inner.definition));
        } else if (static_cast<uint32_t>(memberLayout->getSize(kUniform)) != mapped.size) {
            Fail(
                "gpu types: {}.{} occupies {} bytes but its spelling spans {}; only tight layouts map",
                structName,
                memberName,
                memberLayout->getSize(kUniform),
                mapped.size
            );
        }

        const uint32_t footprint = mapped.size;
        const uint32_t cxxAlign  = mapped.cxxAlign;
        cursor                   = offset + footprint;
        def.align                = std::max(def.align, slangAlign);
        def.fields.push_back(Field {
            .name       = std::string {memberName},
            .type       = std::move(mapped),
            .offset     = offset,
            .size       = footprint,
            .slangAlign = slangAlign,
            .cxxAlign   = cxxAlign,
        });
    }
    if (def.fields.empty()) {
        Fail("gpu types: {} declares no members; an empty struct has no C++ spelling here", structName);
    }
    def.size = AlignUp(cursor, def.align);
    if (def.size > cursor) {
        def.fields.push_back(Field {
            .name       = std::format("_pad{}", padIndex),
            .offset     = cursor,
            .size       = def.size - cursor,
            .slangAlign = 1,
            .cxxAlign   = 1,
            .isPad      = true,
        });
    }
    return result;
}

constexpr std::string_view kStructsPreamble = R"ZHLN(// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// GENERATED by tools/zshader from the gpu_abi module, compiled in-process. Do not edit.
//
// The GPU ABI, as C++: every struct gpu_abi.slang wraps, with the offsets,
// sizes, padding and lanes of the compiled module. One definition per concept,
// and this is it -- there is no hand-written half to alias, so a Slang edit
// re-emits this file on the next build and nothing can drift behind the
// shader's back.
//
// The definitions are the whole contract: every gap is an explicit `_padN` run
// of the reflected size and every lane carries the alignment Slang gave it, so
// each struct's extent is stated by the text rather than inferred from the host
// compiler's packing rules. There is nothing left for a static_assert to add --
// removing a member or reshaping one is a Slang edit that re-emits the struct,
// which is the point.
//
// Nothing under include/ names what is in here. The engine authors its own
// terms (Zahlen/ParticleEmitterDesc.hpp, Zahlen/Render/FrameData.hpp) and
// src/render/GpuPack.cpp packs them into these structs. The enum declarations
// here are the GPU-side, Slang-derived wire types; packing converts the engine's
// domain enums explicitly at that boundary.

#pragma once

#include <Jolt/Jolt.h> // First: Jolt wants Jolt.h before any of its own headers
#include <Jolt/Math/Float2.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Mat44.h>

#include "pipeline/PushDataLayout.hpp" // Vk::HeapPushDataLayout: kScenePushLayout's type

#include <array>
#include <cstdint>
#include <string_view>

namespace ZHLN::Slang {

)ZHLN";

auto EmitField(std::string& out, const Field& field, std::string_view structName) -> void {
    if (field.isPad) {
        std::format_to(
            std::back_inserter(out), "    uint8_t {}[{}]; // offset {}, size {}\n", field.name, field.size, field.offset, field.size
        );
        return;
    }
    const Spelling    spelling  = Render(field.type, structName, field.name);
    const std::string alignAttr = field.slangAlign > field.cxxAlign ? std::format("alignas({}) ", field.slangAlign) : std::string {};
    std::format_to(std::back_inserter(out), "    {}{} {}", alignAttr, spelling.base, field.name);
    for (uint32_t dim: spelling.dims) {
        std::format_to(std::back_inserter(out), "[{}]", dim);
    }
    std::format_to(std::back_inserter(out), "; // offset {}, size {}\n", field.offset, field.size);
}

// One enum per Slang enum the ABI reaches. The case list and its numbering are
// the reflection's; the fixed `: uint32_t` base is what ReflectEnum demanded of
// Slang (it fails on any other storage), so the width is settled before the
// declaration is printed and needs no assert behind it.
auto EmitEnum(std::string& out, const EnumDef& def) -> void {
    std::format_to(std::back_inserter(out), "// Reflected from Slang; do not define a parallel GPU enum.\nenum class {} : uint32_t {{\n", def.name);
    for (const EnumValue& value: def.values) {
        std::format_to(std::back_inserter(out), "    {} = {}u,\n", value.name, value.value);
    }
    std::format_to(std::back_inserter(out), "}};\n\n");
}

// One frame-address relationship: a DescriptorHeapPushData word paired with
// the GlobalSceneRegistry buffer binding it carries. Both halves come from
// reflection -- the words from the generated struct's 8-byte members in
// declaration order, the bindings from the scene ParameterBlock's buffer
// members in declaration order -- so neither side is restated here, and the
// declaration order is the whole association. The rows carry both names, so a
// reorder of either declaration regenerates this file visibly; nothing at
// compile time re-pins the pairs, which is why the two declarations are meant
// to grow at the end rather than in the middle.
struct FrameAddressRow {
    std::string pushField {};
    std::string registryField {};
    uint32_t    binding = 0;
    uint32_t    offset  = 0;
    bool        uniformBuffer = false;
};

auto EmitFrameAddresses(std::string& out, std::span<const FrameAddressRow> rows) -> void {
    std::format_to(std::back_inserter(out), "\n// The frame-address run: field placement is generated (above), CPU resource\n// selection is engine policy (src/render/RenderFrame.cpp).\nenum class FrameAddressField : uint32_t {{\n");
    for (const FrameAddressRow& row: rows) {
        std::format_to(std::back_inserter(out), "    {},\n", row.pushField);
    }
    std::format_to(std::back_inserter(out), "}};\n\nstruct FrameAddressMeta {{\n    FrameAddressField   field;\n    ::std::string_view  pushField;\n    ::std::string_view  registryField;\n    uint32_t            binding;\n    uint32_t            offset;\n    bool                uniformBuffer;\n}};\n\ninline constexpr uint32_t kFrameAddressCount = {};\ninline constexpr ::std::array<FrameAddressMeta, {}> kFrameAddressMeta = {{{{\n", rows.size(), rows.size());
    for (const FrameAddressRow& row: rows) {
        std::format_to(
            std::back_inserter(out),
            "    {{FrameAddressField::{}, \"{}\", \"{}\", {}u, {}u, {}}},\n",
            row.pushField,
            row.pushField,
            row.registryField,
            row.binding,
            row.offset,
            row.uniformBuffer ? "true" : "false"
        );
    }
    std::format_to(std::back_inserter(out), "}}}};\n");
}

// The scene push blob's shape, read off the struct walk: where the 8-byte frame
// addresses sit inside DescriptorHeapPushData (in declaration order, one row per
// scene buffer binding), the descriptor-index word that follows them, and the
// furthest byte a push writes. src/render/GpuAbi.cpp exports it as
// GpuAbi::kScenePushLayout, so the numbers the heap writer pushes with are the
// ones the shader's own declaration implies.
struct ScenePushLayout {
    std::vector<uint32_t> frameAddressOffsets {};
    uint32_t              heapIndexOffset = 0;
    uint32_t              requiredSize    = 0;
};

auto EmitScenePushLayout(std::string& out, const ScenePushLayout& layout) -> void {
    std::string offsets;
    for (size_t index = 0; index < layout.frameAddressOffsets.size(); ++index) {
        std::format_to(std::back_inserter(offsets), "{}{}u", index == 0 ? "" : ", ", layout.frameAddressOffsets[index]);
    }
    std::format_to(
        std::back_inserter(out),
        "\n// DescriptorHeapPushData, as the heap writer writes it: the frame addresses,\n"
        "// the descriptor-index word the heap writer patches per dispatch, and how far a\n"
        "// push reaches. src/render/GpuAbi.cpp exports this; nothing re-reads the module's\n"
        "// bytes to derive it.\n"
        "inline constexpr ::ZHLN::Vk::HeapPushDataLayout kScenePushLayout = {{\n"
        "    .frameAddressOffsets = {{{}}},\n"
        "    .addressCount        = {}u,\n"
        "    .heapIndexOffset     = {}u,\n"
        "    .requiredSize        = {}u,\n"
        "}};\n",
        offsets,
        layout.frameAddressOffsets.size(),
        layout.heapIndexOffset,
        layout.requiredSize
    );
}

auto EmitStructsHeader(
    std::span<const StructDef> defs, std::span<const EnumDef> enums, std::span<const FrameAddressRow> frameRows, const ScenePushLayout& scenePush
) -> std::string {
    std::string text {kStructsPreamble};
    for (const EnumDef& def: enums) {
        EmitEnum(text, def);
    }
    bool first = true;
    for (const StructDef& def: defs) {
        std::format_to(std::back_inserter(text), "{}struct alignas({}) {} {{\n", first ? "" : "\n", def.align, def.name);
        for (const Field& field: def.fields) {
            EmitField(text, field, def.name);
        }
        std::format_to(std::back_inserter(text), "}};\n");
        first = false;
    }
    EmitFrameAddresses(text, frameRows);
    EmitScenePushLayout(text, scenePush);
    std::format_to(std::back_inserter(text), "\n}} // namespace ZHLN::Slang\n");
    return text;
}

} // namespace

// Compiles the gpu_abi module through the shared compile (CompileSlangEntry)
// -- the same session shape the catalog mode reflects through (SPIR-V, the
// entry-point name preserved, column-major matrices), differing only in the
// codegen levels it sets explicitly (maximal debug info, no optimization) --
// walks the reflected wrapper, and writes the header plus the module's own
// SPIR-V. Codegen runs before the walk so a Slang failure leaves no half
// a pair behind; both files land through WriteFileIfChanged, untouched when
// the compile reproduces them.
void RunGpuTypesMode(const GpuTypesOptions& options) {
    if (options.searchPaths.empty()) {
        Fail("gpu types: no Slang search paths; the module will not resolve its imports");
    }

    // .defines stays empty (the mode takes no -D flags) and .stage names the
    // [shader("compute")] CSMain gpu_abi.slang declares, so the lookup checks
    // what the old findEntryPointByName took on faith.
    const SlangProgram program = CompileSlangEntry(SlangCompileArgs{
        .module            = options.module,
        .entry             = kEntryPoint,
        .stage             = SLANG_STAGE_COMPUTE,
        .searchPaths       = options.searchPaths,
        .debugInfoLevel    = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
        .optimizationLevel = SLANG_OPTIMIZATION_LEVEL_NONE,
    });

    Slang::ComPtr<slang::IBlob> diagnostics;
    Slang::ComPtr<slang::IBlob> code;
    if (SLANG_FAILED(program.linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef())) ||
        code.get() == nullptr) {
        Fail("gpu types: Slang cannot emit code for '{}': {}", options.module, DiagText(diagnostics.get()));
    }
    CheckDiagnostics(options.module, "emitting code for", diagnostics.get());
    const std::string spvBytes {static_cast<const char*>(code->getBufferPointer()), code->getBufferSize()};
    if (spvBytes.empty()) {
        Fail("gpu types: Slang emitted no SPIR-V for '{}'", options.module);
    }

    slang::ProgramLayout* programLayout = program.linked->getLayout();
    if (programLayout == nullptr) {
        Fail("gpu types: Slang linked '{}' but produced no layout", options.module);
    }
    slang::VariableLayoutReflection* globals = programLayout->getGlobalParamsVarLayout();
    if (globals == nullptr) {
        Fail("gpu types: '{}' declares no globals; it is not the gpu_abi module", options.module);
    }
    slang::TypeLayoutReflection* globalsLayout = globals->getTypeLayout();
    if (globalsLayout == nullptr) {
        Fail("gpu types: '{}' declares globals without a layout; it is not the gpu_abi module", options.module);
    }
    slang::VariableLayoutReflection* abi = nullptr;
    slang::VariableLayoutReflection* scene = nullptr;
    for (unsigned i: std::views::iota(0u, globalsLayout->getFieldCount())) {
        slang::VariableLayoutReflection* field = globalsLayout->getFieldByIndex(i);
        const char*                       name  = field->getName();
        if (name != nullptr && std::string_view {name} == kAbiGlobal) {
            abi = field;
        }
        if (name != nullptr && std::string_view {name} == kSceneGlobal) {
            scene = field;
        }
    }
    if (abi == nullptr) {
        Fail("gpu types: '{}' declares no '{}' global; it is not the gpu_abi module", options.module, kAbiGlobal);
    }
    slang::TypeLayoutReflection* abiLayout = abi->getTypeLayout();
    if (abiLayout->getType()->getKind() != slang::TypeReflection::Kind::ConstantBuffer) {
        Fail(
            "gpu types: '{}.{}' is a {}, not the ConstantBuffer wrapper",
            options.module,
            kAbiGlobal,
            KindName(abiLayout->getType()->getKind())
        );
    }
    slang::TypeLayoutReflection* wrapperLayout = abiLayout->getElementTypeLayout();
    const char*                  wrapperName    = wrapperLayout->getName();
    if (wrapperName == nullptr || std::string_view {wrapperName} != kAbiWrapper) {
        Fail(
            "gpu types: '{}.{}' wraps '{}', not the '{}' wrapper",
            options.module,
            kAbiGlobal,
            wrapperName != nullptr ? wrapperName : "<unnamed>",
            kAbiWrapper
        );
    }

    std::vector<EnumDef>   enums;
    std::vector<StructDef> nested;
    std::vector<StructDef> structs;
    for (unsigned i: std::views::iota(0u, wrapperLayout->getFieldCount())) {
        slang::VariableLayoutReflection* member       = wrapperLayout->getFieldByIndex(i);
        slang::TypeLayoutReflection*    memberLayout = member->getTypeLayout();
        const char*                     name         = member->getName();
        if (name == nullptr || name[0] == '\0') {
            Fail("gpu types: '{}' wraps an unnamed member; a struct has nothing to be checked by", options.module);
        }
        if (memberLayout->getType()->getKind() != slang::TypeReflection::Kind::Struct) {
            Fail(
                "gpu types: '{}.{}' holds a {}, not a struct; the wrapper holds structs only",
                options.module,
                name,
                KindName(memberLayout->getType()->getKind())
            );
        }
        const char* typeName = memberLayout->getName();
        if (typeName == nullptr || typeName[0] == '\0') {
            Fail("gpu types: '{}.{}' is an unnamed struct; a struct has nothing to be checked by", options.module, name);
        }
        StructBuildResult built = BuildStruct(typeName, memberLayout, enums);
        std::ranges::move(built.nested, std::back_inserter(nested));
        structs.push_back(std::move(built.definition));
    }

    // The frame-address relationship, generated rather than hand-paired: the
    // DescriptorHeapPushData 8-byte words (from the struct walk above) against
    // the scene ParameterBlock's buffer bindings (name + binding index from
    // this reflection), both in declaration order. A count mismatch fails here;
    // the emitted rows carry both names, so a reorder of either declaration
    // regenerates them visibly and lands in review with the Slang edit that
    // caused it.
    if (scene == nullptr) {
        Fail("gpu types: '{}' declares no '{}' global; the frame-address metadata has no binding side", options.module, kSceneGlobal);
    }
    slang::TypeLayoutReflection* sceneLayout = scene->getTypeLayout();
    if (sceneLayout->getType()->getKind() != slang::TypeReflection::Kind::ParameterBlock) {
        Fail(
            "gpu types: '{}.{}' is a {}, not the ParameterBlock wrapper",
            options.module,
            kSceneGlobal,
            KindName(sceneLayout->getType()->getKind())
        );
    }
    slang::TypeLayoutReflection* registryLayout = sceneLayout->getElementTypeLayout();

    const StructDef* heapDef = nullptr;
    for (const StructDef& def: structs) {
        if (def.name == kHeapStruct) {
            heapDef = &def;
        }
    }
    if (heapDef == nullptr) {
        Fail("gpu types: '{}' wraps no '{}'; the frame-address metadata has no field side", options.module, kHeapStruct);
    }

    std::vector<FrameAddressRow> frameRows;
    uint32_t                     registryIndex = 0;
    for (unsigned i: std::views::iota(0u, registryLayout->getFieldCount())) {
        slang::VariableLayoutReflection* member      = registryLayout->getFieldByIndex(i);
        slang::TypeLayoutReflection*     memberLayout = member->getTypeLayout();
        const slang::TypeReflection::Kind kind        = memberLayout->getType()->getKind();
        const SlangResourceShape          shape       = memberLayout->getType()->getResourceShape();
        const bool uniformBuffer = kind == slang::TypeReflection::Kind::ConstantBuffer;
        const bool storageBuffer = kind == slang::TypeReflection::Kind::Resource
                                   && (shape & SLANG_RESOURCE_BASE_SHAPE_MASK) == SLANG_STRUCTURED_BUFFER;
        if (!uniformBuffer && !storageBuffer) {
            continue;
        }
        if (registryIndex >= static_cast<uint32_t>(heapDef->fields.size())) {
            break;
        }
        const char* memberName = member->getName();
        if (memberName == nullptr || memberName[0] == '\0') {
            Fail("gpu types: '{}.{}' member {} is unnamed; the frame-address rows name both halves", options.module, kSceneGlobal, i);
        }
        frameRows.push_back(FrameAddressRow {
            .pushField      = {},
            .registryField  = memberName,
            .binding        = member->getBindingIndex(),
            .offset         = 0,
            .uniformBuffer  = uniformBuffer,
        });
        ++registryIndex;
    }

    {
        std::vector<const Field*> words;
        for (const Field& field: heapDef->fields) {
            if (!field.isPad && field.size == sizeof(uint64_t)) {
                words.push_back(&field);
            }
        }
        if (words.size() != frameRows.size()) {
            Fail(
                "gpu types: DescriptorHeapPushData declares {} 8-byte words but {} has {} buffer bindings; the frame-address run is one word per binding",
                words.size(),
                kSceneGlobal,
                frameRows.size()
            );
        }
        for (size_t i = 0; i < words.size(); ++i) {
            frameRows[i].pushField = words[i]->name;
            frameRows[i].offset    = words[i]->offset;
        }
    }

    // The scene push blob, derived from the struct walk rather than from a
    // parse of the module's bytes: the addresses are the rows above, and the
    // descriptor-index word is the first 4-byte-or-wider member that follows
    // the last one (DescriptorHeapPushData's `heapIndex`). The failures here
    // are the ones the heap writer would otherwise hit at runtime, after the
    // mapping had already claimed an offset nothing occupies.
    ScenePushLayout scenePush;
    scenePush.frameAddressOffsets.reserve(frameRows.size());
    for (const FrameAddressRow& row: frameRows) {
        scenePush.frameAddressOffsets.push_back(row.offset);
    }
    if (scenePush.frameAddressOffsets.empty()) {
        Fail("gpu types: '{}' seats no 8-byte frame addresses; the heap writer has nothing to push", kHeapStruct);
    }
    // HeapPushDataLayout::kMaxAddresses. The tool has no RHI include path and
    // this is the one number it has to agree with the container on.
    constexpr size_t kMaxFrameAddresses = 16;
    if (scenePush.frameAddressOffsets.size() > kMaxFrameAddresses) {
        Fail(
            "gpu types: '{}' seats {} frame addresses; Vk::HeapPushDataLayout holds {}",
            kHeapStruct,
            scenePush.frameAddressOffsets.size(),
            kMaxFrameAddresses
        );
    }
    const uint32_t afterAddresses = scenePush.frameAddressOffsets.back() + static_cast<uint32_t>(sizeof(uint64_t));
    uint32_t       heapIndexOffset = std::numeric_limits<uint32_t>::max();
    for (const Field& field: heapDef->fields) {
        if (field.isPad || field.offset < afterAddresses || field.size < sizeof(uint32_t) || (field.offset % alignof(uint32_t)) != 0) {
            continue;
        }
        heapIndexOffset = field.offset < heapIndexOffset ? field.offset : heapIndexOffset;
    }
    if (heapIndexOffset == std::numeric_limits<uint32_t>::max()) {
        Fail(
            "gpu types: '{}' declares no 4-byte member after its last frame address; the heap writer patches a descriptor index nothing holds",
            kHeapStruct
        );
    }
    scenePush.heapIndexOffset = heapIndexOffset;
    scenePush.requiredSize    = heapIndexOffset + static_cast<uint32_t>(sizeof(uint32_t));

    std::vector<StructDef> defs;
    defs.reserve(nested.size() + structs.size());
    for (std::vector<StructDef>* bucket: {&nested, &structs}) {
        for (StructDef& candidate: *bucket) {
            const auto existing = std::ranges::find_if(defs, [&candidate](const StructDef& d) { return d.name == candidate.name; });
            if (existing == defs.end()) {
                defs.push_back(std::move(candidate));
                continue;
            }
            // A type reachable twice -- a wrapper member another payload also
            // holds (ParticleEmitterParams under ComputePushConstants and the
            // emitter member) -- is one type and is emitted once. The same
            // name with a different shape is a lie rather than a duplicate,
            // and fails here.
            if (existing->size != candidate.size || existing->fields.size() != candidate.fields.size()) {
                Fail("gpu types: '{}' is reachable twice with different shapes ({} vs {} bytes)", candidate.name, existing->size, candidate.size);
            }
            for (size_t i = 0; i < existing->fields.size(); ++i) {
                const Field& a = existing->fields[i];
                const Field& b = candidate.fields[i];
                if (a.name != b.name || a.offset != b.offset || a.size != b.size || a.isPad != b.isPad) {
                    Fail("gpu types: '{}' is reachable twice and member {} does not agree between the paths", candidate.name, i);
                }
            }
        }
    }
    WriteFileIfChanged(options.outStructs, EmitStructsHeader(defs, enums, frameRows, scenePush));
    WriteFileIfChanged(options.outSpv, spvBytes);
    std::println("zshader: gpu types: {} struct(s), {} enum(s) from {}", defs.size(), enums.size(), options.module);
}

} // namespace ZHLN::ZShader
