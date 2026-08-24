// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SlangTypeLayout.hpp"
#include <cstring>
#include <spirv_reflect.h>
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

[[nodiscard]] bool TypeNameMatches(const char* candidate, std::string_view want) noexcept {
    if (candidate == nullptr || candidate[0] == '\0' || want.empty()) {
        return false;
    }
    if (want == candidate) {
        return true;
    }
    const char* dot = std::strrchr(candidate, '.');
    return dot != nullptr && want == (dot + 1);
}

[[nodiscard]] auto LayoutFromBlock(const SpvReflectBlockVariable& block) noexcept -> SlangTypeLayout {
    SlangTypeLayout layout;
    layout.size      = block.size;
    layout.alignment = 0;
    layout.fields.reserve(block.member_count);
    for (uint32_t i = 0; i < block.member_count; ++i) {
        const auto& member = block.members[i];
        if (member.name == nullptr) {
            continue;
        }
        layout.fields.push_back({.name = member.name, .offset = member.offset, .size = member.size});
    }
    return layout;
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

    if (TypeNameMatches(block.name, typeName) || TypeNameMatches(blockType, typeName) || TypeNameMatches(elemType, typeName)) {
        out = LayoutFromBlock(block);
        return;
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
    if (result->size == 0) {
        return std::unexpected(SpirvLayoutError::EmptyLayout);
    }
    return std::move(*result);
}

} // namespace ZHLN::Vk
