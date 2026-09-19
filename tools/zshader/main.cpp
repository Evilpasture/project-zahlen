// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zshader/main.cpp
//
// zshader: reflects cooked SPIR-V into the renderer's shader catalog.
//
// Runs after the shader cooks, once per build in which a module changed, and
// writes two files:
//
//   * ShaderBindings.hpp -- pure text. One type per module (its entry point, its
//     stage, its binding lists with descriptor types, sets and binding numbers,
//     its push-constant layout) and one `Vk::ShaderSet<...>` alias per pass.
//     Nothing in it is bytes, so every translation unit that writes descriptors
//     parses kilobytes and checks names against type lists rather than walking
//     SPIR-V;
//
//   * ShaderBytecode.cpp -- the translation unit that `#embed`s every cooked
//     module the catalog carries. It defines each module's `Bytes()`, the byte
//     spans the rest of the engine reads, and one `static_assert` per module
//     holding the generated lists against the module's own bytes with the
//     independent reader in ShaderProgram.hpp. A module the catalog does not
//     carry -- gpu_abi.slang, which no pipeline loads -- is not passed to
//     `--bytes` at all: its only reader is the compile-time ABI check, which
//     embeds it (src/render/GpuAbi.hpp, where the GPU ABI check lives).
//
// The tool is deliberately the second opinion, not the only one: SPIRV-Reflect is
// what the renderer already reflects with at pipeline creation, and the
// assertion in the generated source is what keeps this tool honest.
//
// Usage:
//
//   zshader --out-header <path> --out-source <path>
//           --bytes <MACRO>=<module.spv> ...            (every cooked module)
//           --module <Type>=<MACRO> ...                 (the catalog's modules)
//           --blob <Name>=<file> ...                    (bytes that are not shaders)
//           --set <Set>=<Type>,<Type> ...               (one alias per pass)
//
// Exit code 0 on success, 1 with a message on stderr otherwise. Files are only
// rewritten when their content changes, so an untouched shader does not
// recompile the translation units that include the header.

#include "spirv_reflect.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Member {
    std::string name;
    uint32_t    offset = 0;
    uint32_t    size   = 0;
};

struct Descriptor {
    std::string name;
    uint32_t    set     = 0;
    uint32_t    binding = 0;
    std::string type; // "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE"
    bool        sampler = false;
};

struct PushBlock {
    std::string          name;
    uint32_t             size       = 0;
    uint32_t             paddedSize = 0;
    std::vector<Member>  members;
};

struct Module {
    std::string              macro;    // SHADER_LIGHTING_SLANG_PS_PATH
    std::string              symbol;   // shader_lighting_slang_ps_path
    std::string              path;     // the cooked .spv, as the build named it
    std::string              type;     // catalog type name, empty for bytes only
    uint32_t                 byteSize = 0;
    std::string              entryPoint;
    std::string              stage; // "VK_SHADER_STAGE_FRAGMENT_BIT"
    bool                     reflected = false;
    std::vector<Descriptor>  resources;
    std::vector<Descriptor>  samplers;
    std::vector<PushBlock>   pushes;
};

/// One cooked module, or one data blob, as the generated files see it: a symbol
/// to name it by and the file the bytes come from.
struct BytesInput {
    std::string symbol;
    std::string path;
    uint32_t    size = 0;
};

struct Options {
    std::string              outHeader;
    std::string              outSource;
    std::vector<Module>      modules;   // one per --bytes, in the order given
    std::vector<BytesInput>  blobs;     // one per --blob, in the order given
    std::map<std::string, std::string> catalog; // type name -> macro
    std::vector<std::pair<std::string, std::vector<std::string>>> sets;
};

/// Every byte input, modules first: the order the generated file embeds them in.
std::vector<BytesInput> BytesInputsOf(const Options& options) {
    std::vector<BytesInput> inputs;
    inputs.reserve(options.modules.size() + options.blobs.size());
    for (const Module& module: options.modules) {
        inputs.push_back(BytesInput {.symbol = module.symbol, .path = module.path, .size = module.byteSize});
    }
    for (const BytesInput& blob: options.blobs) {
        inputs.push_back(blob);
    }
    return inputs;
}

void Fail(const std::string& message) {
    std::fprintf(stderr, "zshader: %s\n", message.c_str());
    std::exit(1);
}

std::string ToSymbol(const std::string& macro) {
    std::string symbol;
    symbol.reserve(macro.size());
    for (const char c: macro) {
        symbol.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return symbol;
}

/// True for a name a C++ declaration can carry.
bool IsIdentifier(const std::string& name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) != 0 || name[0] == '_')) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    });
}

/// The descriptor type as the engine spells it. A sampler is a sampler because
/// the module says so, not because of where it sits.
const char* DescriptorTypeName(SpvReflectDescriptorType type) {
    switch (type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            return "VK_DESCRIPTOR_TYPE_SAMPLER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            return "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return "VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return "VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC";
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return "VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT";
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR";
        default:
            return nullptr;
    }
}

const char* StageName(SpvReflectShaderStageFlagBits stage) {
    switch (stage) {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
            return "VK_SHADER_STAGE_VERTEX_BIT";
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            return "VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT";
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            return "VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT";
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:
            return "VK_SHADER_STAGE_GEOMETRY_BIT";
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
            return "VK_SHADER_STAGE_FRAGMENT_BIT";
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:
            return "VK_SHADER_STAGE_COMPUTE_BIT";
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT:
            return "VK_SHADER_STAGE_TASK_BIT_EXT";
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT:
            return "VK_SHADER_STAGE_MESH_BIT_EXT";
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:
            return "VK_SHADER_STAGE_RAYGEN_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return "VK_SHADER_STAGE_ANY_HIT_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return "VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:
            return "VK_SHADER_STAGE_MISS_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return "VK_SHADER_STAGE_INTERSECTION_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR:
            return "VK_SHADER_STAGE_CALLABLE_BIT_KHR";
        default:
            return nullptr;
    }
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        Fail("cannot open " + path);
    }
    const std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        Fail("cannot read " + path);
    }
    return bytes;
}

/// A path as a C++ string literal: forward slashes (both toolchains take them)
/// and escaped backslashes if one survived.
std::string AsLiteral(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (const char c: path) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// Reflects one module. Fills entry point, stage, descriptors and push blocks,
/// and refuses anything the engine's model cannot carry: a module is one entry
/// point, and every descriptor must have a name the tool can hold the write side
/// against.
void Reflect(Module& module) {
    const std::vector<uint8_t> bytes = ReadFile(module.path);
    module.byteSize                  = static_cast<uint32_t>(bytes.size());

    SpvReflectShaderModule reflected {};
    const SpvReflectResult result = spvReflectCreateShaderModule(bytes.size(), bytes.data(), &reflected);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        Fail("SPIRV-Reflect could not read " + module.path + " (result " + std::to_string(static_cast<int>(result)) + ")");
    }

    if (reflected.entry_point_count != 1) {
        spvReflectDestroyShaderModule(&reflected);
        Fail(module.path + " declares " + std::to_string(reflected.entry_point_count) + " entry points; the catalog's modules declare exactly one");
    }
    const SpvReflectEntryPoint& entry = reflected.entry_points[0];
    module.entryPoint                 = entry.name == nullptr ? "" : entry.name;
    const char* stage                 = StageName(entry.shader_stage);
    if (stage == nullptr) {
        spvReflectDestroyShaderModule(&reflected);
        Fail(module.path + " is compiled for a stage this engine does not build pipelines for");
    }
    module.stage = stage;

    for (uint32_t i = 0; i < reflected.descriptor_binding_count; ++i) {
        const SpvReflectDescriptorBinding& binding = reflected.descriptor_bindings[i];
        const char* typeName = DescriptorTypeName(binding.descriptor_type);
        if (typeName == nullptr) {
            spvReflectDestroyShaderModule(&reflected);
            Fail(module.path + " declares descriptor type " + std::to_string(static_cast<int>(binding.descriptor_type)) + ", which the engine has no heap for");
        }
        if (binding.name == nullptr || binding.name[0] == '\0') {
            spvReflectDestroyShaderModule(&reflected);
            Fail(module.path + " declares an unnamed descriptor; a write has nothing to match it by");
        }
        Descriptor descriptor;
        descriptor.name    = binding.name;
        descriptor.set     = binding.set;
        descriptor.binding = binding.binding;
        descriptor.type    = typeName;
        descriptor.sampler = binding.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER;
        (descriptor.sampler ? module.samplers : module.resources).push_back(std::move(descriptor));
    }
    const auto byBinding = [](const Descriptor& a, const Descriptor& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    };
    std::sort(module.resources.begin(), module.resources.end(), byBinding);
    std::sort(module.samplers.begin(), module.samplers.end(), byBinding);

    for (uint32_t i = 0; i < reflected.push_constant_block_count; ++i) {
        const SpvReflectBlockVariable& block = reflected.push_constant_blocks[i];
        PushBlock push;
        push.name       = block.name == nullptr ? "" : block.name;
        push.size       = block.size;
        push.paddedSize = block.padded_size;
        for (uint32_t m = 0; m < block.member_count; ++m) {
            const SpvReflectBlockVariable& member = block.members[m];
            push.members.push_back(Member {
                .name   = member.name == nullptr ? "" : member.name,
                .offset = member.offset,
                .size   = member.size,
            });
        }
        module.pushes.push_back(std::move(push));
    }

    spvReflectDestroyShaderModule(&reflected);
    module.reflected = true;
}

/// `--bytes MACRO=path`
Module ParseBytes(const std::string& argument) {
    const size_t split = argument.find('=');
    if (split == std::string::npos || split == 0 || split + 1 >= argument.size()) {
        Fail("--bytes wants MACRO=<module.spv>, got " + argument);
    }
    Module module;
    module.macro  = argument.substr(0, split);
    module.path   = argument.substr(split + 1);
    module.symbol = ToSymbol(module.macro);
    if (!IsIdentifier(module.symbol)) {
        Fail("--bytes " + module.macro + " is not a C++ identifier: the macro name is what names the byte span");
    }
    return module;
}

/// `--set Name=Type,Type`
std::pair<std::string, std::vector<std::string>> ParseSet(const std::string& argument) {
    const size_t split = argument.find('=');
    if (split == std::string::npos || split == 0 || split + 1 >= argument.size()) {
        Fail("--set wants Name=<Type>[,<Type>...], got " + argument);
    }
    std::pair<std::string, std::vector<std::string>> set;
    set.first = argument.substr(0, split);
    std::string remainder = argument.substr(split + 1);
    size_t      start     = 0;
    while (start <= remainder.size()) {
        const size_t comma = remainder.find(',', start);
        const size_t end   = comma == std::string::npos ? remainder.size() : comma;
        if (end > start) {
            set.second.push_back(remainder.substr(start, end - start));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (set.second.empty()) {
        Fail("--set " + set.first + " names no modules");
    }
    return set;
}

std::string SlotList(const std::vector<Descriptor>& descriptors, bool sampler, const std::string& indent) {
    if (descriptors.empty()) {
        return "Vk::BindingList<>";
    }
    std::ostringstream out;
    out << "Vk::BindingList<\n";
    for (size_t i = 0; i < descriptors.size(); ++i) {
        const Descriptor& descriptor = descriptors[i];
        out << indent << "    Vk::Declared::";
        if (sampler) {
            out << "SamplerSlot<\"" << descriptor.name << "\", " << descriptor.set << ", " << descriptor.binding << ">";
        } else {
            out << "ResourceSlot<\"" << descriptor.name << "\", " << descriptor.type << ", " << descriptor.set << ", " << descriptor.binding << ">";
        }
        if (i + 1 != descriptors.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << ">";
    return out.str();
}

std::string EmitHeader(const Options& options) {
    std::ostringstream out;
    out << "// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me\n";
    out << "// SPDX-License-Identifier: GPL-3.0-or-later\n";
    out << "\n";
    out << "// File: <build>/generated_shaders/ShaderBindings.hpp\n";
    out << "//\n";
    out << "// GENERATED by tools/zshader from the cooked SPIR-V. Do not edit.\n";
    out << "//\n";
    out << "// One type per module: its entry point, its stage, the bindings it declares with\n";
    out << "// their descriptor types and numbers, and the layout of its push-constant block.\n";
    out << "// One `Vk::ShaderSet<...>` alias per pass, which is what a descriptor write names.\n";
    out << "// No bytes: the modules themselves live in the generated ShaderBytecode.cpp, which\n";
    out << "// is also where these lists are held against those bytes at compile time.\n";
    out << "\n";
    out << "#pragma once\n";
    out << "\n";
    out << "#include \"Rendering.hpp\" // the RHI implementation headers\n";
    out << "#include \"pipeline/ShaderProgram.hpp\" // Vk::ShaderProgram, Vk::ShaderSet\n";
    out << "\n";
    out << "#include <cstdint>\n";
    out << "#include <span>\n";
    out << "#include <string_view>\n";
    out << "\n";
    out << "namespace ZHLN::ShaderLib {\n";
    out << "\n";
    out << "/// One cooked module's bytes, defined in the generated ShaderBytecode.cpp --\n";
    out << "/// the only translation unit in the project that #embeds them. Defined once,\n";
    out << "/// so the image holds one copy of each module however many sites read it.\n";
    for (const BytesInput& input: BytesInputsOf(options)) {
        out << "extern const std::span<const uint8_t> " << input.symbol << "; // " << input.path << "\n";
    }
    out << "\n";
    out << "} // namespace ZHLN::ShaderLib\n";
    out << "\n";
    out << "namespace ZHLN::Shaders {\n";
    out << "\n";
    out << "/// The modules themselves. A set named after a pass (Lighting) is a different\n";
    out << "/// declaration from the module it wraps, which is why the two live in separate\n";
    out << "/// namespaces.\n";
    out << "namespace Modules {\n";
    for (const auto& [type, macro]: options.catalog) {
        const auto module = std::find_if(options.modules.begin(), options.modules.end(), [&](const Module& candidate) {
            return candidate.macro == macro;
        });
        if (module == options.modules.end() || !module->reflected) {
            Fail("catalog type " + type + " names macro " + macro + ", which no --bytes input carries");
        }
        out << "\n";
        out << "struct " << type << " {\n";
        out << "    using Resources = " << SlotList(module->resources, false, "        ") << ";\n";
        out << "    using Samplers  = " << SlotList(module->samplers, true, "        ") << ";\n";
        out << "\n";
        out << "    /// The entry point the module declares, and the stage it was compiled\n";
        out << "    /// for: what a pipeline is built with, read out of the module.\n";
        out << "    static constexpr const char*           EntryPoint = \"" << module->entryPoint << "\";\n";
        out << "    static constexpr VkShaderStageFlagBits Stage      = " << module->stage << ";\n";
        out << "\n";
        out << "    /// The cooked module these declarations came from, for a hot reload and\n";
        out << "    /// for a reader that wants to compare against the file itself.\n";
        out << "    static constexpr const char* Path     = \"" << AsLiteral(module->path) << "\";\n";
        out << "    static constexpr uint32_t    ByteSize = " << module->byteSize << ";\n";
        if (!module->pushes.empty()) {
            const PushBlock& push = module->pushes.front();
            out << "\n";
            out << "    /// The push-constant block the module declares: the size a push range\n";
            out << "    /// needs and the members it is made of. The engine's own push struct is\n";
            out << "    /// written by hand (it carries VkDeviceAddress and engine math types);\n";
            out << "    /// this is what it can be held against.\n";
            if (module->pushes.size() > 1) {
                out << "    /// (The module declares " << module->pushes.size() << " blocks; the first is\n";
                out << "    /// what this build's pipelines take.)\n";
            }
            out << "    static constexpr uint32_t   PushSize = " << push.paddedSize << ";\n";
            out << "    static constexpr Vk::PushMember Push[] = {\n";
            for (const Member& member: push.members) {
                out << "        {\"" << member.name << "\", " << member.offset << ", " << member.size << "},\n";
            }
            out << "    };\n";
        }
        out << "\n";
        out << "    [[nodiscard]] static auto Bytes() -> std::span<const uint8_t>;\n";
        out << "};\n";
    }
    out << "\n";
    out << "} // namespace Modules\n";
    out << "\n";
    out << "/// One set per descriptor block: the modules whose bindings that block\n";
    out << "/// serves. A write site names the set, and the set is what the compile-time\n";
    out << "/// checks read.\n";
    for (const auto& [name, members]: options.sets) {
        out << "using " << name << " = Vk::ShaderSet<";
        for (size_t i = 0; i < members.size(); ++i) {
            out << "Modules::" << members[i];
            if (i + 1 != members.size()) {
                out << ", ";
            }
        }
        out << ">;\n";
    }
    out << "\n";
    out << "} // namespace ZHLN::Shaders\n";
    return out.str();
}

std::string EmitSource(const Options& options) {
    std::ostringstream out;
    out << "// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me\n";
    out << "// SPDX-License-Identifier: GPL-3.0-or-later\n";
    out << "\n";
    out << "// File: <build>/generated_shaders/ShaderBytecode.cpp\n";
    out << "//\n";
    out << "// GENERATED by tools/zshader. Do not edit.\n";
    out << "//\n";
    out << "// The translation unit that #embeds every cooked module the catalog carries, and\n";
    out << "// place where ShaderBindings.hpp is held against the modules it was generated\n";
    out << "// from: every module's own bytes, walked here by SpirvBindings.hpp -- a reader\n";
    out << "// that shares nothing with the SPIRV-Reflect the tool used -- must agree with\n";
    out << "// the entry point, the stage and the binding lists the header states. A tool\n";
    out << "// that reflected a module wrongly, and a header left stale by a rebuild, both\n";
    out << "// fail to compile here instead of writing a descriptor nobody declared.\n";
    out << "\n";
    out << "#include \"ShaderBindings.hpp\"\n";
    out << "\n";
    out << "#include \"pipeline/ShaderProgram.hpp\"\n";
    out << "\n";
    out << "#include <cstdint>\n";
    out << "#include <span>\n";
    out << "\n";
    out << "namespace {\n";
    out << "\n";
    for (const BytesInput& input: BytesInputsOf(options)) {
        out << "constexpr uint8_t " << input.symbol << "_bytes[] = {\n";
        out << "#embed \"" << AsLiteral(input.path) << "\"\n";
        out << "};\n";
        out << "static_assert(sizeof(" << input.symbol << "_bytes) == " << input.size << ");\n";
        out << "\n";
    }
    out << "} // namespace\n";
    out << "\n";
    out << "namespace ZHLN::ShaderLib {\n";
    out << "\n";
    for (const BytesInput& input: BytesInputsOf(options)) {
        out << "const std::span<const uint8_t> " << input.symbol << " {" << input.symbol << "_bytes};\n";
    }
    out << "\n";
    out << "} // namespace ZHLN::ShaderLib\n";
    out << "\n";
    out << "namespace ZHLN::Shaders::Modules {\n";
    out << "\n";
    for (const auto& [type, macro]: options.catalog) {
        const auto module = std::find_if(options.modules.begin(), options.modules.end(), [&](const Module& candidate) {
            return candidate.macro == macro;
        });
        out << "auto " << type << "::Bytes() -> std::span<const uint8_t> {\n";
        out << "    return ZHLN::ShaderLib::" << module->symbol << ";\n";
        out << "}\n";
        out << "\n";
    }
    out << "} // namespace ZHLN::Shaders::Modules\n";
    out << "\n";
    for (const auto& [type, macro]: options.catalog) {
        const auto module = std::find_if(options.modules.begin(), options.modules.end(), [&](const Module& candidate) {
            return candidate.macro == macro;
        });
        out << "// " << type << " <- " << module->path << "\n";
        out << "static_assert(\n";
        out << "    ZHLN::Vk::ModuleMatchesBytes<ZHLN::Shaders::Modules::" << type << ">(" << module->symbol << "_bytes),\n";
        out << "    \"ShaderBindings.hpp does not describe the module it was generated from (tools/zshader): regenerate, or fix the tool\"\n";
        out << ");\n";
        out << "\n";
    }
    return out.str();
}

bool WriteIfChanged(const std::string& path, const std::string& content) {
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            std::ostringstream buffer;
            buffer << existing.rdbuf();
            if (buffer.str() == content) {
                std::printf("zshader: %s is up to date\n", path.c_str());
                return false;
            }
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Fail("cannot write " + path);
    }
    file << content;
    if (!file) {
        Fail("cannot write " + path);
    }
    std::printf("zshader: wrote %s\n", path.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto          next   = [&]() -> std::string {
            if (i + 1 >= argc) {
                Fail(argument + " wants a value");
            }
            return argv[++i];
        };
        if (argument == "--out-header") {
            options.outHeader = next();
        } else if (argument == "--out-source") {
            options.outSource = next();
        } else if (argument == "--bytes") {
            Module module = ParseBytes(next());
            for (const Module& existing: options.modules) {
                if (existing.macro == module.macro) {
                    Fail("--bytes " + module.macro + " given twice");
                }
            }
            Reflect(module);
            options.modules.push_back(std::move(module));
        } else if (argument == "--module") {
            const std::string value = next();
            const size_t      split = value.find('=');
            if (split == std::string::npos || split == 0 || split + 1 >= value.size()) {
                Fail("--module wants Type=<MACRO>, got " + value);
            }
            const std::string type  = value.substr(0, split);
            const std::string macro = value.substr(split + 1);
            if (!options.catalog.emplace(type, macro).second) {
                Fail("--module " + type + " given twice");
            }
        } else if (argument == "--blob") {
            const std::string value = next();
            const size_t      split = value.find('=');
            if (split == std::string::npos || split == 0 || split + 1 >= value.size()) {
                Fail("--blob wants NAME=<file>, got " + value);
            }
            BytesInput blob;
            blob.symbol = value.substr(0, split);
            blob.path   = value.substr(split + 1);
            if (!IsIdentifier(blob.symbol)) {
                Fail("--blob " + blob.symbol + " is not a C++ identifier: it names the span the bytes are read through");
            }
            blob.size = static_cast<uint32_t>(ReadFile(blob.path).size());
            options.blobs.push_back(std::move(blob));
        } else if (argument == "--set") {
            options.sets.push_back(ParseSet(next()));
        } else {
            Fail("unknown argument " + argument);
        }
    }

    if (options.outHeader.empty() || options.outSource.empty()) {
        Fail("--out-header and --out-source are both required");
    }
    if (options.modules.empty()) {
        Fail("no --bytes inputs: there is nothing to reflect");
    }
    for (const auto& [name, members]: options.sets) {
        for (const std::string& member: members) {
            if (options.catalog.find(member) == options.catalog.end()) {
                Fail("--set " + name + " names " + member + ", which is not a --module");
            }
        }
    }

    WriteIfChanged(options.outHeader, EmitHeader(options));
    WriteIfChanged(options.outSource, EmitSource(options));
    std::printf(
        "zshader: %zu module(s), %zu blob(s), %zu catalog type(s), %zu set(s)\n", options.modules.size(), options.blobs.size(), options.catalog.size(), options.sets.size()
    );
    return 0;
}
