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
// to JPH::Float2/Float3/Float4, float4x4 to JPH::Mat44, a fixed array to
// std::array of its element -- with [CxxEnum] the one annotation left, naming
// the engine enum behind an int/uint scalar. There is no table in this file
// restating either half, so a Slang edit carries its spelling with it and
// nothing here can skew a layout behind the shader's back.
//
// Five of the structs are not emitted here at all. Particle,
// ParticleEmitterParams, MeshParticleEmitterParams, Light and FrameUniforms
// are hand-written in include/Zahlen/Render/RenderData.hpp -- one definition
// per concept, in the public header -- and this file aliases them and asserts
// their size and every member offset against the reflection. That assert is
// the proof the hand-written structs *are* the ABI: a disagreement is a
// compile error naming the member, not a skew the host uploads.
//
// One compile produces two outputs: GeneratedGpuTypes.hpp, the structs, and
// the module's own SPIR-V, which src/render/GpuAbi.hpp embeds and re-reads
// with its independent consteval parser. Both halves agree by construction,
// and the check still fails against the module's bytes rather than our walk.
//
// Two things the walk refuses to derive, and fails on instead of guessing:
//
//   * GPUMeshlet is skipped ([CxxSkip]): its ABI is the raw word protocol in
//     instance_data.slang's fetchMeshlet (coneAxis at byte 44), which no
//     std140/std430 declaration of consecutive float3s can spell (Slang seats
//     it at 48). The hand-written ZHLN::GPUMeshlet stays authoritative.
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
#include <memory>
#include <optional>
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

// The cxx_abi.slang vocabulary this walk answers to. Unknown names fail in
// ReadMemberAttributes/IsSkipped, so a vocabulary edit lands here loudly.
// [CxxArray], [CxxCArray], [CxxQuat] and [CxxDefault] used to live here: they
// existed only to respell a type the Slang kind already names, and the host
// structs now carry their own defaults, so every one of them is gone.
constexpr std::string_view kAttrEnum = "CxxEnum";
constexpr std::string_view kAttrSkip = "CxxSkip";

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
// carries no annotation -- the surrounding offsets and the struct sizeof
// prove it -- which is what distinguishes it from Slang's own _paddingCenter
// and _pad members (real fields, checked like any other).
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

// What the member asked for. One annotation is left: [CxxEnum], which names
// the engine enum a uint/int scalar is. Every name outside the vocabulary
// fails here, so a misspelled or future annotation is loud rather than
// silently defaulted.
struct MemberSpelling {
    std::optional<std::string> enumName {};
};

auto ReadStringArg(
    slang::UserAttribute* attr, std::string_view structName, std::string_view memberName, std::string_view attrName
) -> std::string {
    const uint32_t args = static_cast<uint32_t>(attr->getArgumentCount());
    if (args != 1) {
        Fail("gpu types: {}.{} carries [{}] with {} arguments; it takes exactly one", structName, memberName, attrName, args);
    }
    size_t      size = 0;
    const char* text = attr->getArgumentValueString(0, &size);
    if (text == nullptr) {
        Fail("gpu types: {}.{} carries [{}] with a non-string argument", structName, memberName, attrName);
    }
    return std::string {text, size};
}

auto ReadMemberAttributes(slang::VariableReflection* var, std::string_view structName, std::string_view memberName) -> MemberSpelling {
    MemberSpelling spelling;
    for (unsigned i: std::views::iota(0u, var->getUserAttributeCount())) {
        slang::UserAttribute* attr = var->getUserAttributeByIndex(i);
        const char*             raw  = attr->getName();
        if (raw == nullptr) {
            Fail("gpu types: {}.{} carries an unnamed annotation", structName, memberName);
        }
        const std::string_view name = raw;
        if (name == kAttrEnum) {
            spelling.enumName = ReadStringArg(attr, structName, memberName, name);
        } else {
            Fail("gpu types: {}.{} carries [{}], which is not a cxx_abi annotation", structName, memberName, name);
        }
    }
    return spelling;
}

// True when the struct opts out of emission. Enumerates every attribute on
// the way, so a struct-level annotation outside the one-word vocabulary is
// loud too.
auto IsSkipped(std::string_view structName, slang::TypeReflection* type) -> bool {
    bool skipped = false;
    for (unsigned i: std::views::iota(0u, type->getUserAttributeCount())) {
        const char* raw = type->getUserAttributeByIndex(i)->getName();
        if (raw == nullptr) {
            Fail("gpu types: {} carries an unnamed annotation", structName);
        }
        if (std::string_view {raw} != kAttrSkip) {
            Fail("gpu types: {} carries [{}], which is not a cxx_abi struct annotation", structName, raw);
        }
        skipped = true;
    }
    return skipped;
}

// The member's alignment as Slang seats it: every alignas in the header
// answers to this number, so a non-positive one fails rather than emitting
// a struct the asserts cannot mean.
auto ReadAlignment(std::string_view structName, std::string_view memberName, slang::TypeLayoutReflection* layout) -> uint32_t {
    const int32_t align = layout->getAlignment(kUniform);
    if (align <= 0) {
        Fail("gpu types: {}.{} reports an alignment of {}; every member seats on a positive boundary", structName, memberName, align);
    }
    return static_cast<uint32_t>(align);
}

// The Slang-kind-to-C++ mapping for one member, which is now the whole
// story: a float vector is its Jolt pod lane type, a float4x4 is JPH::Mat44,
// a fixed array is std::array of its element, and the scalars are stdint. No
// arm holds the result against anything but the size Slang reported, so the
// shader stays the measure of every size.
auto MapMember(
    std::string_view structName, std::string_view memberName, slang::TypeLayoutReflection* layout, const MemberSpelling& spelling
) -> CxxType {
    using Kind = slang::TypeReflection::Kind;
    slang::TypeReflection* type   = layout->getType();
    const Kind               kind   = type->getKind();
    const auto               scalar = type->getScalarType();
    const bool               isFloat32 = scalar == slang::TypeReflection::ScalarType::Float32;

    if (kind == Kind::Struct) {
        if (spelling.enumName.has_value()) {
            Fail("gpu types: {}.{} is a struct; [CxxEnum] applies to int and uint scalars", structName, memberName);
        }
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
        if (spelling.enumName.has_value()) {
            Fail("gpu types: {}.{} is a matrix; [CxxEnum] applies to int and uint scalars", structName, memberName);
        }
        return {.form = CxxForm::Mat, .spelling = std::string {kMat4}, .size = 64, .cxxAlign = 16};
    }
    if (kind == Kind::Array) {
        slang::TypeLayoutReflection* elementLayout = layout->getElementTypeLayout();
        const Kind                     elementKind   = elementLayout->getType()->getKind();
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
        if (spelling.enumName.has_value()) {
            Fail("gpu types: {}.{} is an array; [CxxEnum] applies to int and uint scalars", structName, memberName);
        }
        // The element maps by the same table, so a repeat of a float4 is a
        // std::array of the pod lane type -- no second spelling for repeats.
        CxxType element = MapMember(structName, memberName, elementLayout, MemberSpelling {});
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
        if (!isFloat32) {
            Fail("gpu types: {}.{} is not a float vector; only float vectors map", structName, memberName);
        }
        const uint32_t components = static_cast<uint32_t>(type->getElementCount());
        if (components < 2 || components > 4) {
            Fail("gpu types: {}.{} has {} vector components; only 2, 3 and 4 map", structName, memberName, components);
        }
        if (spelling.enumName.has_value()) {
            Fail("gpu types: {}.{} is a vector; [CxxEnum] applies to int and uint scalars", structName, memberName);
        }
        // The pod lane type: same lanes as the shader, layout 4-byte aligned
        // and 4/8/12/16 bytes wide, which is what the reflected offsets were
        // measured with. A float4 landing mid-16-byte-boundary is the shader's
        // own padding rule, and BuildStruct puts the _pad there.
        const std::string_view lanes = components == 2 ? kFloat2 : (components == 3 ? kFloat3 : kFloat4);
        return {.form = CxxForm::Vec, .spelling = std::string {lanes}, .size = 4 * components, .cxxAlign = 4};
    }
    if (kind == Kind::Scalar) {
        using Scalar = slang::TypeReflection::ScalarType;
        if (spelling.enumName.has_value()) {
            if (scalar != Scalar::Int32 && scalar != Scalar::UInt32) {
                Fail("gpu types: {}.{} carries [CxxEnum] but is not an int or uint scalar", structName, memberName);
            }
            return {.form = CxxForm::Enum, .spelling = *spelling.enumName, .size = 4, .cxxAlign = 4};
        }
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
auto BuildStruct(std::string_view structName, slang::TypeLayoutReflection* layout, std::vector<StructDef>& nestedOut) -> StructDef {
    std::vector<slang::VariableLayoutReflection*> members;
    for (unsigned i: std::views::iota(0u, layout->getFieldCount())) {
        members.push_back(layout->getFieldByIndex(i));
    }
    std::ranges::sort(members, {}, [](slang::VariableLayoutReflection* member) { return member->getOffset(kUniform); });

    StructDef def {.name = std::string {structName}};
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
        slang::VariableReflection*   var           = member->getVariable();
        if (var == nullptr) {
            Fail("gpu types: {}.{} has no variable reflection; its annotations are unreadable", structName, memberName);
        }
        const MemberSpelling spelling    = ReadMemberAttributes(var, structName, memberName);
        const uint32_t         slangAlign = ReadAlignment(structName, memberName, memberLayout);
        CxxType                mapped     = MapMember(structName, memberName, memberLayout, spelling);

        // A nested struct recurses through the same builder; anything else
        // takes its mapped spelling, held against the member size Slang
        // reported. The nested footprint is the recomputed one (AlignUp,
        // never the padded getSize), and a skipped type named as a member
        // fails: nothing may hold what the host never emits.
        if (mapped.form == CxxForm::Struct) {
            const char* nestedName = memberLayout->getName();
            if (nestedName == nullptr || nestedName[0] == '\0') {
                Fail("gpu types: {}.{} is an unnamed struct; a nested type has nothing to be checked by", structName, memberName);
            }
            if (IsSkipped(nestedName, memberLayout->getType())) {
                Fail("gpu types: {}.{} names the [CxxSkip] '{}'; nothing may hold what the host never emits", structName, memberName, nestedName);
            }
            StructDef inner   = BuildStruct(nestedName, memberLayout, nestedOut);
            mapped.spelling = inner.name;
            mapped.size     = inner.size;
            mapped.cxxAlign = inner.align;
            nestedOut.push_back(std::move(inner));
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
    return def;
}

constexpr std::string_view kStructsPreamble = R"ZHLN(// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// GENERATED by tools/zshader from the gpu_abi module, compiled in-process. Do not edit.
//
// Slang's half of the GPU ABI, as C++: every struct gpu_abi.slang wraps that
// the engine does not define itself, with the offsets, sizes and padding of
// the compiled module. A Slang edit re-emits this file on the next build.
//
// The structs the engine *does* define -- Particle, ParticleEmitterParams,
// MeshParticleEmitterParams, Light and FrameUniforms, all in
// include/Zahlen/Render/RenderData.hpp -- are aliased here, and the asserts
// below hold the hand-written definition against the reflection: same size,
// every member at the offset Slang seated it. A host compiler that packs
// differently, or a hand edit that moves a member, fails here instead of
// uploading skewed buffers. GPUMeshlet is intentionally absent -- its ABI is
// the raw word protocol in instance_data.slang's fetchMeshlet, which no
// std140/std430 declaration of consecutive float3s can spell (see
// GpuTypes.cpp).

#pragma once

#include <Jolt/Jolt.h> // First: Jolt wants Jolt.h before any of its own headers
#include <Jolt/Math/Float2.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Mat44.h>
#include <Zahlen/Render/RenderData.hpp> // the hand-written half this file aliases
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

namespace ZHLN::GeneratedGpu {

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

auto EmitInventory(std::string& out, const std::vector<StructDef>& defs) -> void {
    std::format_to(
        std::back_inserter(out),
        "\n// The ABI inventory, for the GpuAbi walk (src/render/GpuAbi.hpp): every struct\n"
        "// above with the module-side name the lookup answers to.\n"
        "template <typename T> struct SlangName;\n"
    );
    for (const StructDef& def: defs) {
        std::format_to(
            std::back_inserter(out),
            "template <> struct SlangName<{}> {{ static constexpr ::std::string_view value = \"{}\"; }};\n",
            def.name,
            def.name
        );
    }
    std::format_to(std::back_inserter(out), "using AllGpuTypes = ::std::tuple<");
    bool first = true;
    for (const StructDef& def: defs) {
        std::format_to(std::back_inserter(out), "{}{}", first ? "" : ", ", def.name);
        first = false;
    }
    std::format_to(std::back_inserter(out), ">;\n\n}} // namespace ZHLN::GeneratedGpu\n");
}

// The structs the engine defines by hand, in include/Zahlen/Render/RenderData.hpp.
// For these the header emits an alias and keeps the asserts; for every other
// struct it emits the body, since nothing else spells it.
constexpr std::string_view kHostStructs[] {
    "Particle", "ParticleEmitterParams", "MeshParticleEmitterParams", "Light", "FrameUniforms"
};

auto IsHostStruct(std::string_view name) -> bool {
    return std::ranges::find(kHostStructs, name) != std::end(kHostStructs);
}

auto EmitStructsHeader(const std::vector<StructDef>& defs) -> std::string {
    std::string text {kStructsPreamble};
    bool        first = true;
    for (const StructDef& def: defs) {
        if (IsHostStruct(def.name)) {
            std::format_to(
                std::back_inserter(text),
                "{}\n// Hand-written in <Zahlen/Render/RenderData.hpp>; the asserts below are the contract.\nusing {} = ::ZHLN::{};\n",
                first ? "" : "\n",
                def.name,
                def.name
            );
        } else {
            std::format_to(std::back_inserter(text), "{}struct alignas({}) {} {{\n", first ? "" : "\n", def.align, def.name);
            for (const Field& field: def.fields) {
                EmitField(text, field, def.name);
            }
            std::format_to(std::back_inserter(text), "}};\n");
        }
        first = false;
        std::format_to(
            std::back_inserter(text),
            "static_assert(sizeof({}) == {}, \"GeneratedGpu::{} does not occupy what Slang reported; the host compiler packs "
            "differently\");\n",
            def.name,
            def.size,
            def.name
        );
        for (const Field& field: def.fields) {
            if (field.isPad) {
                continue;
            }
            std::format_to(
                std::back_inserter(text),
                "static_assert(offsetof({}, {}) == {}, \"GeneratedGpu::{}::{} sits where Slang did not put it\");\n",
                def.name,
                field.name,
                field.offset,
                def.name,
                field.name
            );
        }
    }
    EmitInventory(text, defs);
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
    for (unsigned i: std::views::iota(0u, globalsLayout->getFieldCount())) {
        slang::VariableLayoutReflection* field = globalsLayout->getFieldByIndex(i);
        const char*                       name  = field->getName();
        if (name != nullptr && std::string_view {name} == kAbiGlobal) {
            abi = field;
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
        if (IsSkipped(typeName, memberLayout->getType())) {
            continue;
        }
        structs.push_back(BuildStruct(typeName, memberLayout, nested));
    }

    std::vector<StructDef> defs;
    defs.reserve(nested.size() + structs.size());
    std::ranges::move(nested, std::back_inserter(defs));
    std::ranges::move(structs, std::back_inserter(defs));
    WriteFileIfChanged(options.outStructs, EmitStructsHeader(defs));
    WriteFileIfChanged(options.outSpv, spvBytes);
    std::println("zshader: gpu types: {} struct(s) from {}", defs.size(), options.module);
}

} // namespace ZHLN::ZShader
