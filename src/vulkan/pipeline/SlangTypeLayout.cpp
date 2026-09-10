// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SlangTypeLayout.hpp"
#include <Zahlen/Core/Math.hpp>
#include <algorithm>
#include <cstring>
#include <spirv_reflect.h>
#include <string_view>
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

[[nodiscard]] auto TypeAlign(const SpvReflectTypeDescription& type) noexcept -> uint32_t {
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_REF) != 0) {
        return 8;
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_ARRAY) != 0) {
        const SpvReflectTypeDescription* elem = type.struct_type_description;
        if (elem == nullptr && type.member_count > 0) {
            elem = &type.members[0];
        }
        return std::max(16u, elem != nullptr ? TypeAlign(*elem) : 16u);
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX) != 0) {
        return 16;
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_VECTOR) != 0) {
        const uint32_t count = type.traits.numeric.vector.component_count;
        if (count >= 3) {
            return 16;
        }
        if (count == 2) {
            return 8;
        }
        return std::max(1u, type.traits.numeric.scalar.width / 8u);
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT) != 0) {
        uint32_t alignment = 1;
        for (uint32_t i = 0; i < type.member_count; ++i) {
            alignment = std::max(alignment, TypeAlign(type.members[i]));
        }
        return alignment;
    }
    if ((type.type_flags & (SPV_REFLECT_TYPE_FLAG_INT | SPV_REFLECT_TYPE_FLAG_FLOAT | SPV_REFLECT_TYPE_FLAG_BOOL)) != 0) {
        return std::max(1u, type.traits.numeric.scalar.width / 8u);
    }
    return 4;
}

[[nodiscard]] auto TypeSize(const SpvReflectTypeDescription& type) noexcept -> uint32_t {
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_REF) != 0) {
        return 8;
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_ARRAY) != 0) {
        const uint32_t count = type.traits.array.dims_count > 0 ? type.traits.array.dims[0] : 0;
        if (type.traits.array.stride > 0 && count > 0) {
            return type.traits.array.stride * count;
        }
        const SpvReflectTypeDescription* elem = type.struct_type_description;
        if (elem == nullptr && type.member_count > 0) {
            elem = &type.members[0];
        }
        return (elem != nullptr && count > 0) ? TypeSize(*elem) * count : 0;
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX) != 0) {
        const auto& matrix = type.traits.numeric.matrix;
        if (matrix.stride > 0) {
            return matrix.stride * matrix.column_count;
        }
        const uint32_t scalar = type.traits.numeric.scalar.width / 8u;
        return scalar * matrix.row_count * matrix.column_count;
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_VECTOR) != 0) {
        return (type.traits.numeric.scalar.width / 8u) * type.traits.numeric.vector.component_count;
    }
    if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT) != 0) {
        return 0;
    }
    if ((type.type_flags & (SPV_REFLECT_TYPE_FLAG_INT | SPV_REFLECT_TYPE_FLAG_FLOAT | SPV_REFLECT_TYPE_FLAG_BOOL)) != 0) {
        return type.traits.numeric.scalar.width / 8u;
    }
    return 0;
}

[[nodiscard]] auto LayoutFromBlock(const SpvReflectBlockVariable& block) noexcept -> SlangTypeLayout;

[[nodiscard]] auto FieldSize(const SpvReflectBlockVariable& member) noexcept -> uint32_t {
    if (member.member_count > 0) {
        return LayoutFromBlock(member).size;
    }
    if (member.type_description != nullptr) {
        if (const uint32_t size = TypeSize(*member.type_description); size > 0) {
            return size;
        }
    }
    return member.size;
}

[[nodiscard]] auto LayoutFromBlock(const SpvReflectBlockVariable& block) noexcept -> SlangTypeLayout {
    SlangTypeLayout layout;
    layout.fields.reserve(block.member_count);
    uint32_t end = 0;
    for (uint32_t i = 0; i < block.member_count; ++i) {
        const auto& member = block.members[i];
        const uint32_t size = FieldSize(member);
        if (member.name != nullptr) {
            layout.fields.push_back({.name = member.name, .offset = member.offset, .size = size});
        }
        end = std::max(end, member.offset + size);
    }
    layout.alignment = block.type_description != nullptr ? TypeAlign(*block.type_description) : 1;
    layout.size      = ZHLN::Math::AlignUp(end, layout.alignment);
    if (layout.size == 0) {
        layout.size = block.size;
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

} // namespace

auto ReflectTypeLayout(const void* spirv, size_t sizeBytes, std::string_view typeName) noexcept -> std::expected<SlangTypeLayout, ZHLN::Error> {
    if (spirv == nullptr || sizeBytes == 0 || typeName.empty()) {
        return std::unexpected(SpirvLayoutError::InvalidArguments);
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
