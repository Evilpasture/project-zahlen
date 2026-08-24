// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SlangTypeLayout.hpp"
#include <algorithm>
#include <cstring>
#include <spirv_reflect.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZHLN::Vk {

auto SlangTypeLayout::FieldOffset(std::string_view name) const noexcept -> std::optional<uint32_t> {
    for (const auto& field: fields) {
        if (field.name == name) {
            return field.offset;
        }
    }
    return std::nullopt;
}

auto SlangTypeLayout::FieldSize(std::string_view name) const noexcept -> std::optional<uint32_t> {
    for (const auto& field: fields) {
        if (field.name == name) {
            return field.size;
        }
    }
    return std::nullopt;
}

namespace {

constexpr uint32_t kSpvMagic             = 0x07230203u;
constexpr uint32_t kOpName               = 5;
constexpr uint32_t kOpMemberName         = 6;
constexpr uint32_t kOpTypeInt            = 21;
constexpr uint32_t kOpTypeFloat          = 22;
constexpr uint32_t kOpTypeVector         = 23;
constexpr uint32_t kOpTypeMatrix         = 24;
constexpr uint32_t kOpConstant           = 43;
constexpr uint32_t kOpSpecConstant       = 48;
constexpr uint32_t kOpTypeArray          = 28;
constexpr uint32_t kOpTypeRuntimeArray   = 29;
constexpr uint32_t kOpTypeStruct         = 30;
constexpr uint32_t kOpTypePointer        = 32;
constexpr uint32_t kOpDecorate           = 71;
constexpr uint32_t kOpMemberDecorate     = 72;
constexpr uint32_t kDecorationArrayStride  = 6;
constexpr uint32_t kDecorationMatrixStride = 7;
constexpr uint32_t kDecorationOffset       = 35;

[[nodiscard]] bool TypeNameMatches(std::string_view candidate, std::string_view want) noexcept {
    if (candidate.empty() || want.empty()) {
        return false;
    }
    if (candidate == want) {
        return true;
    }
    const auto dot = candidate.rfind('.');
    if (dot != std::string_view::npos && candidate.substr(dot + 1) == want) {
        return true;
    }
    // slangc emits a layout-specialized copy as TypeName_std140 / _std430 / _scalar.
    return candidate.size() > want.size() + 1 && candidate.starts_with(want) && candidate[want.size()] == '_';
}

[[nodiscard]] bool TypeNameMatches(const char* candidate, std::string_view want) noexcept {
    return candidate != nullptr && TypeNameMatches(std::string_view(candidate), want);
}

[[nodiscard]] auto LayoutFromBlock(const SpvReflectBlockVariable& block) noexcept -> SlangTypeLayout {
    SlangTypeLayout layout;
    layout.size      = block.size;
    layout.alignment = 0;
    layout.fields.reserve(block.member_count);
    uint32_t maxEnd = block.size;
    for (uint32_t i = 0; i < block.member_count; ++i) {
        const auto& member = block.members[i];
        if (member.name == nullptr) {
            continue;
        }
        layout.fields.push_back({.name = member.name, .offset = member.offset, .size = member.size});
        maxEnd = std::max(maxEnd, member.offset + member.size);
    }
    if (layout.size == 0) {
        layout.size = maxEnd;
    }
    if (layout.size == 0 && block.array.stride > 0) {
        layout.size = block.array.stride;
    }
    return layout;
}

[[nodiscard]] bool AcceptLayout(const SlangTypeLayout& layout) noexcept {
    return layout.size > 0 && !layout.fields.empty();
}

void VisitBlock(const SpvReflectBlockVariable& block, std::string_view typeName, std::optional<SlangTypeLayout>& out) noexcept {
    if (out) {
        return;
    }

    const char* blockType = nullptr;
    const char* elemType  = nullptr;
    if (block.type_description != nullptr) {
        blockType = block.type_description->type_name;
        if (block.type_description->struct_type_description != nullptr) {
            elemType = block.type_description->struct_type_description->type_name;
        }
    }

    const bool nameMatch =
        TypeNameMatches(block.name, typeName) || TypeNameMatches(blockType, typeName) || TypeNameMatches(elemType, typeName);

    if (nameMatch) {
        auto layout = LayoutFromBlock(block);
        if (AcceptLayout(layout)) {
            out = std::move(layout);
            return;
        }
        if (block.member_count == 1) {
            auto inner = LayoutFromBlock(block.members[0]);
            if (AcceptLayout(inner)) {
                out = std::move(inner);
                return;
            }
        }
    }

    for (uint32_t i = 0; i < block.member_count; ++i) {
        VisitBlock(block.members[i], typeName, out);
        if (out) {
            return;
        }
    }
}

struct SpvType {
    uint32_t                 op     = 0;
    uint32_t                 width  = 0;
    uint32_t                 count  = 0;
    uint32_t                 elem   = 0;
    uint32_t                 stride = 0;
    std::vector<uint32_t>    members;
    std::string              name;
    std::vector<std::string> memberNames;
    std::vector<uint32_t>    memberOffsets;
};

[[nodiscard]] auto ReadSpvString(const uint32_t* words, uint32_t remaining) -> std::string {
    std::string out;
    for (uint32_t i = 0; i < remaining; ++i) {
        const uint32_t word = words[i];
        for (uint32_t b = 0; b < 4; ++b) {
            const char c = static_cast<char>((word >> (8u * b)) & 0xFFu);
            if (c == '\0') {
                return out;
            }
            out.push_back(c);
        }
    }
    return out;
}

uint32_t TypeAlign(const std::unordered_map<uint32_t, SpvType>& types, uint32_t id);
uint32_t TypeSize(const std::unordered_map<uint32_t, SpvType>& types, uint32_t id);

[[nodiscard]] uint32_t AlignUp(uint32_t value, uint32_t alignment) noexcept {
    if (alignment <= 1) {
        return value;
    }
    return (value + alignment - 1u) / alignment * alignment;
}

uint32_t TypeAlign(const std::unordered_map<uint32_t, SpvType>& types, uint32_t id) {
    const auto it = types.find(id);
    if (it == types.end()) {
        return 1;
    }
    const auto& t = it->second;
    switch (t.op) {
        case kOpTypeInt:
        case kOpTypeFloat:
            return std::max(1u, t.width / 8u);
        case kOpTypeVector:
            return t.count >= 3 ? 16u : (t.count == 2 ? 8u : TypeAlign(types, t.elem));
        case kOpTypeMatrix:
            return 16;
        case kOpTypeArray:
        case kOpTypeRuntimeArray:
            return std::max(16u, TypeAlign(types, t.elem));
        case kOpTypePointer:
            return 8;
        case kOpTypeStruct: {
            uint32_t alignment = 1;
            for (uint32_t member: t.members) {
                alignment = std::max(alignment, TypeAlign(types, member));
            }
            return alignment;
        }
        default:
            return 4;
    }
}

uint32_t TypeSize(const std::unordered_map<uint32_t, SpvType>& types, uint32_t id) {
    const auto it = types.find(id);
    if (it == types.end()) {
        return 0;
    }
    const auto& t = it->second;
    switch (t.op) {
        case kOpTypeInt:
        case kOpTypeFloat:
            return t.width / 8u;
        case kOpTypeVector:
            return TypeSize(types, t.elem) * t.count;
        case kOpTypeMatrix:
            if (t.stride > 0) {
                return t.stride * t.count;
            }
            return TypeSize(types, t.elem) * t.count;
        case kOpTypeArray:
            if (t.stride > 0) {
                return t.stride * t.count;
            }
            return TypeSize(types, t.elem) * t.count;
        case kOpTypeRuntimeArray:
            return 0;
        case kOpTypePointer:
            return 8;
        case kOpTypeStruct: {
            uint32_t end = 0;
            for (size_t i = 0; i < t.members.size(); ++i) {
                const uint32_t off = (i < t.memberOffsets.size()) ? t.memberOffsets[i] : end;
                end                = std::max(end, off + TypeSize(types, t.members[i]));
            }
            return AlignUp(end, TypeAlign(types, id));
        }
        default:
            return 0;
    }
}

[[nodiscard]] auto LayoutFromSpvStruct(uint32_t id, const SpvType& t, const std::unordered_map<uint32_t, SpvType>& types) -> SlangTypeLayout {
    SlangTypeLayout layout;
    layout.fields.reserve(t.members.size());
    uint32_t end = 0;
    for (size_t i = 0; i < t.members.size(); ++i) {
        const uint32_t off  = (i < t.memberOffsets.size()) ? t.memberOffsets[i] : 0;
        const uint32_t size = TypeSize(types, t.members[i]);
        const char*    name = (i < t.memberNames.size() && !t.memberNames[i].empty()) ? t.memberNames[i].c_str() : nullptr;
        if (name != nullptr) {
            layout.fields.push_back({.name = name, .offset = off, .size = size});
        }
        end = std::max(end, off + size);
    }
    layout.size      = AlignUp(end, TypeAlign(types, id));
    layout.alignment = TypeAlign(types, id);
    return layout;
}

[[nodiscard]] auto ReflectNamedStruct(const uint32_t* words, size_t wordCount, std::string_view typeName) -> std::optional<SlangTypeLayout> {
    if (wordCount < 5 || words[0] != kSpvMagic) {
        return std::nullopt;
    }

    std::unordered_map<uint32_t, SpvType> types;
    std::unordered_map<uint32_t, uint32_t> constants;
    std::unordered_map<uint32_t, std::string> names;

    size_t i = 5;
    while (i < wordCount) {
        const uint32_t first    = words[i];
        const uint32_t wc       = first >> 16;
        const uint32_t op       = first & 0xFFFFu;
        if (wc == 0 || i + wc > wordCount) {
            break;
        }
        const uint32_t* ops = words + i + 1;
        const uint32_t  n   = wc - 1;

        switch (op) {
            case kOpName:
                if (n >= 2) {
                    names[ops[0]] = ReadSpvString(ops + 1, n - 1);
                }
                break;
            case kOpMemberName:
                if (n >= 3) {
                    auto& t = types[ops[0]];
                    const uint32_t member = ops[1];
                    if (t.memberNames.size() <= member) {
                        t.memberNames.resize(member + 1);
                    }
                    t.memberNames[member] = ReadSpvString(ops + 2, n - 2);
                }
                break;
            case kOpDecorate:
                if (n >= 3 && ops[1] == kDecorationArrayStride) {
                    types[ops[0]].stride = ops[2];
                } else if (n >= 3 && ops[1] == kDecorationMatrixStride) {
                    types[ops[0]].stride = ops[2];
                }
                break;
            case kOpMemberDecorate:
                if (n >= 4 && ops[2] == kDecorationOffset) {
                    auto& t = types[ops[0]];
                    const uint32_t member = ops[1];
                    if (t.memberOffsets.size() <= member) {
                        t.memberOffsets.resize(member + 1, 0);
                    }
                    t.memberOffsets[member] = ops[3];
                } else if (n >= 4 && ops[2] == kDecorationMatrixStride) {
                    // Matrix stride lives on the member; apply to the member type if needed later.
                }
                break;
            case kOpTypeInt:
                if (n >= 3) {
                    types[ops[0]] = {.op = op, .width = ops[1]};
                }
                break;
            case kOpTypeFloat:
                if (n >= 2) {
                    types[ops[0]] = {.op = op, .width = ops[1]};
                }
                break;
            case kOpTypeVector:
                if (n >= 3) {
                    types[ops[0]] = {.op = op, .count = ops[2], .elem = ops[1]};
                }
                break;
            case kOpTypeMatrix:
                if (n >= 3) {
                    types[ops[0]] = {.op = op, .count = ops[2], .elem = ops[1]};
                }
                break;
            case kOpTypeArray:
                if (n >= 3) {
                    uint32_t length = 0;
                    if (const auto c = constants.find(ops[2]); c != constants.end()) {
                        length = c->second;
                    }
                    auto& t  = types[ops[0]];
                    t.op     = op;
                    t.elem   = ops[1];
                    t.count  = length;
                }
                break;
            case kOpTypeRuntimeArray:
                if (n >= 2) {
                    types[ops[0]] = {.op = op, .elem = ops[1]};
                }
                break;
            case kOpTypePointer:
                if (n >= 3) {
                    types[ops[0]] = {.op = op, .elem = ops[2]};
                }
                break;
            case kOpTypeStruct:
                if (n >= 1) {
                    auto& t = types[ops[0]];
                    t.op    = op;
                    t.members.assign(ops + 1, ops + n);
                    if (t.memberNames.size() < t.members.size()) {
                        t.memberNames.resize(t.members.size());
                    }
                }
                break;
            case kOpConstant:
            case kOpSpecConstant:
                if (n >= 2) {
                    constants[ops[1]] = (n >= 3) ? ops[2] : 0;
                }
                break;
            default:
                break;
        }
        i += wc;
    }

    for (auto& [id, t]: types) {
        if (auto it = names.find(id); it != names.end()) {
            t.name = it->second;
        }
    }

    std::optional<SlangTypeLayout> best;
    for (const auto& [id, t]: types) {
        if (t.op != kOpTypeStruct || !TypeNameMatches(t.name, typeName)) {
            continue;
        }
        const bool decorated = t.memberOffsets.size() == t.members.size() && !t.members.empty();
        if (!decorated) {
            continue;
        }
        auto layout = LayoutFromSpvStruct(id, t, types);
        if (!AcceptLayout(layout)) {
            continue;
        }
        if (!best || layout.fields.size() >= best->fields.size()) {
            best = std::move(layout);
        }
    }
    return best;
}

} // namespace

auto ReflectTypeLayout(const void* spirv, size_t sizeBytes, std::string_view typeName) noexcept -> std::expected<SlangTypeLayout, ZHLN::Error> {
    if (spirv == nullptr || sizeBytes == 0 || typeName.empty()) {
        return std::unexpected(SpirvLayoutError::InvalidArguments);
    }

    if ((sizeBytes % sizeof(uint32_t)) == 0) {
        auto fromSpv = ReflectNamedStruct(static_cast<const uint32_t*>(spirv), sizeBytes / sizeof(uint32_t), typeName);
        if (fromSpv && AcceptLayout(*fromSpv)) {
            return std::move(*fromSpv);
        }
    }

    SpvReflectShaderModule module;
    if (spvReflectCreateShaderModule(sizeBytes, spirv, &module) != SPV_REFLECT_RESULT_SUCCESS) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }

    std::optional<SlangTypeLayout> result;

    uint32_t bindingCount = 0;
    spvReflectEnumerateDescriptorBindings(&module, &bindingCount, nullptr);
    std::vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
    if (bindingCount > 0) {
        spvReflectEnumerateDescriptorBindings(&module, &bindingCount, bindings.data());
        for (const auto* binding: bindings) {
            if (binding == nullptr) {
                continue;
            }
            VisitBlock(binding->block, typeName, result);
            if (result) {
                break;
            }
        }
    }

    if (!result) {
        uint32_t pushCount = 0;
        spvReflectEnumeratePushConstantBlocks(&module, &pushCount, nullptr);
        std::vector<SpvReflectBlockVariable*> pushes(pushCount);
        if (pushCount > 0) {
            spvReflectEnumeratePushConstantBlocks(&module, &pushCount, pushes.data());
            for (const auto* block: pushes) {
                if (block == nullptr) {
                    continue;
                }
                VisitBlock(*block, typeName, result);
                if (result) {
                    break;
                }
            }
        }
    }

    spvReflectDestroyShaderModule(&module);
    if (!result) {
        return std::unexpected(SpirvLayoutError::TypeNotFound);
    }
    if (!AcceptLayout(*result)) {
        return std::unexpected(SpirvLayoutError::EmptyLayout);
    }
    return std::move(*result);
}

} // namespace ZHLN::Vk
