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
//   * ShaderBytecode.cpp -- the only translation unit in the project that
//     `#embed`s cooked SPIR-V. It defines each module's `Bytes()`, the byte spans
//     the rest of the engine reads, and one `static_assert` per module holding
//     the generated lists against the module's own bytes with the independent
//     reader in ShaderProgram.hpp.
//
// The tool is deliberately the second opinion, not the only one: SPIRV-Reflect is
// what the renderer already reflects with at pipeline creation, and the
// assertion in the generated source is what keeps this tool honest.
//
// Descriptor types and stages are named by reflection rather than by a switch
// per enum (Zahlen/Core/Reflection/Enums.hpp): the compiler knows every
// enumerator the reflector can report, so a SPIRV-Reflect that grows a
// descriptor type or a stage needs no change here, and no enumerator list can
// fall behind either header. What the spelling is here is a prefix rule -- see
// VulkanNameOf -- and what a named value has to satisfy is the engine's business
// (Vk::WriteSourceOfDeclaration for the shapes a write can carry,
// Vk::ShaderStages::Create<...> for the stage a slot accepts).
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

#include <Zahlen/Core/Reflection/Enums.hpp> // ZHLN::Reflect::EnumToString: what the reflector calls a descriptor type or a stage

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <format>
#include <iterator> // std::back_inserter: Emitter builds its text with std::format_to
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// ============================================================================
// Diagnostics
// ============================================================================

/// Stops the build with one formatted line. An unusable module must never become
/// a catalog that names a descriptor nobody declared, so every refusal here is
/// fatal, and it says which input and which value it refused.
template <typename... Args>
[[noreturn]] void Fail(std::format_string<Args...> format, Args&&... args) {
    std::print(stderr, "zshader: ");
    std::println(stderr, format, std::forward<Args>(args)...);
    std::exit(EXIT_FAILURE);
}

// ============================================================================
// The reflected model
// ============================================================================

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

/// The push-constant block whose layout the catalog states: what a push range
/// needs to be wide, and the members that fill it.
struct PushBlock {
    uint32_t            paddedSize = 0;
    std::vector<Member> members;
};

struct Module {
    std::string macro;    // SHADER_LIGHTING_SLANG_PS_PATH
    std::string symbol;   // shader_lighting_slang_ps_path
    std::string path;     // the cooked .spv, as the build named it
    uint32_t    byteSize = 0;
    std::string entryPoint;
    std::string stage; // "VK_SHADER_STAGE_FRAGMENT_BIT"
    bool        reflected = false;

    std::vector<Descriptor> resources;
    std::vector<Descriptor> samplers;
    std::vector<PushBlock>  pushes;
};

/// One cooked module, or one data blob, as the generated files see it: a symbol
/// to name it by and the file the bytes come from.
struct BytesInput {
    std::string symbol;
    std::string path;
    uint32_t    size = 0;
};

struct Options {
    std::string                                                   outHeader;
    std::string                                                   outSource;
    std::vector<Module>                                           modules; // one per --bytes, in the order given
    std::vector<BytesInput>                                       blobs;   // one per --blob, in the order given
    std::map<std::string, std::string>                            catalog; // type name -> macro
    std::vector<std::pair<std::string, std::vector<std::string>>> sets;
};

/// Every byte input, modules first: the order the generated file embeds them in.
[[nodiscard]] auto BytesInputsOf(const Options& options) -> std::vector<BytesInput> {
    std::vector<BytesInput> inputs;
    inputs.reserve(options.modules.size() + options.blobs.size());
    for (const Module& module: options.modules) {
        inputs.push_back(BytesInput {.symbol = module.symbol, .path = module.path, .size = module.byteSize});
    }
    inputs.insert(inputs.end(), options.blobs.begin(), options.blobs.end());
    return inputs;
}

/// The module a catalog entry names.
[[nodiscard]] auto FindModule(const Options& options, std::string_view type, std::string_view macro) -> const Module& {
    const auto module = std::ranges::find(options.modules, macro, &Module::macro);
    if (module == options.modules.end() || !module->reflected) {
        Fail("catalog type {} names macro {}, which no --bytes input carries", type, macro);
    }
    return *module;
}

// ============================================================================
// Names
// ============================================================================

/// Where the reflector's vocabulary sits in Vulkan's: SPIRV-Reflect spells every
/// value it reports with `SPV_REFLECT_` in front of the name Vulkan gives it
/// (`SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE` is
/// `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`). The catalog has to name the enumerator
/// the renderer's own switches name, so the name the compiler hands over is
/// rewritten rather than looked up in a table that would have to be kept in step
/// with two headers.
inline constexpr std::string_view kReflectedPrefix = "SPV_REFLECT_";
inline constexpr std::string_view kVulkanPrefix    = "VK_";

/// The Vulkan spelling of an enumerator name the compiler handed over, or
/// nothing when the name is not spelled the way the reflector spells Vulkan's.
[[nodiscard]] auto VulkanNameOf(std::string_view reflected) -> std::optional<std::string> {
    if (!reflected.starts_with(kReflectedPrefix)) {
        return std::nullopt;
    }
    return std::format("{}{}", kVulkanPrefix, reflected.substr(kReflectedPrefix.size()));
}

/// What a module calls its descriptor type, in the renderer's vocabulary. The
/// name is the compiler's (ZHLN::Reflect::EnumToString), so nothing here
/// enumerates descriptor types, and the call takes a concrete enum type: that is
/// the shape tools/transpile_reflection.py rewrites when the compiler has no
/// reflection, by enumerating the constants of that very type.
[[nodiscard]] auto DescriptorTypeName(SpvReflectDescriptorType type) -> std::optional<std::string> {
    return VulkanNameOf(ZHLN::Reflect::EnumToString(type));
}

/// The same for the stage a module was compiled for.
[[nodiscard]] auto StageName(SpvReflectShaderStageFlagBits stage) -> std::optional<std::string> {
    return VulkanNameOf(ZHLN::Reflect::EnumToString(stage));
}

/// The macro name as a C++ symbol: the generated byte spans are the macro's own
/// spelling in lowercase.
[[nodiscard]] auto ToSymbol(std::string_view macro) -> std::string {
    std::string symbol;
    symbol.reserve(macro.size());
    for (const char character: macro) {
        symbol.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return symbol;
}

/// True for a name a C++ declaration can carry.
[[nodiscard]] auto IsIdentifier(std::string_view name) -> bool {
    if (name.empty() || (std::isalpha(static_cast<unsigned char>(name.front())) == 0 && name.front() != '_')) {
        return false;
    }
    return std::ranges::all_of(name, [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    });
}

/// A path as a C++ string literal: forward slashes (both toolchains take them)
/// and escaped backslashes if one survived.
[[nodiscard]] auto AsLiteral(std::string_view path) -> std::string {
    std::string literal;
    literal.reserve(path.size());
    for (const char character: path) {
        if (character == '\\') {
            literal.append(2, '\\');
        } else if (character == '"') {
            literal.push_back('\\');
            literal.push_back('"');
        } else {
            literal.push_back(character);
        }
    }
    return literal;
}

// ============================================================================
// Input
// ============================================================================

/// The bytes of `path`, or nothing when there is no such file. A file that
/// exists and cannot be read is a refusal, not a "no".
[[nodiscard]] auto ReadFileIfPresent(std::string_view path) -> std::optional<std::vector<uint8_t>> {
    std::ifstream file(std::string {path}, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    std::vector<uint8_t>  bytes(static_cast<size_t>(size < 0 ? 0 : size));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        Fail("cannot read {}", path);
    }
    return bytes;
}

[[nodiscard]] auto ReadFile(std::string_view path) -> std::vector<uint8_t> {
    std::optional<std::vector<uint8_t>> bytes = ReadFileIfPresent(path);
    if (!bytes.has_value()) {
        Fail("cannot open {}", path);
    }
    return *std::move(bytes);
}

/// `NAME=value`, the shape every option argument has.
[[nodiscard]] auto SplitNameValue(std::string_view argument) -> std::optional<std::pair<std::string, std::string>> {
    const size_t split = argument.find('=');
    if (split == std::string_view::npos || split == 0 || split + 1 >= argument.size()) {
        return std::nullopt;
    }
    return std::pair {std::string {argument.substr(0, split)}, std::string {argument.substr(split + 1)}};
}

/// Reflects one module. Fills entry point, stage, descriptors and push blocks,
/// and refuses anything the engine's model cannot carry: a module is one entry
/// point, and every descriptor must have a name the write side can be held
/// against.
void Reflect(Module& module) {
    const std::vector<uint8_t> bytes = ReadFile(module.path);
    module.byteSize                  = static_cast<uint32_t>(bytes.size());

    SpvReflectShaderModule reflected {};
    const SpvReflectResult result = spvReflectCreateShaderModule(bytes.size(), bytes.data(), &reflected);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        Fail("SPIRV-Reflect could not read {} (result {})", module.path, static_cast<int>(result));
    }

    if (reflected.entry_point_count != 1) {
        const uint32_t entries = reflected.entry_point_count;
        spvReflectDestroyShaderModule(&reflected);
        Fail("{} declares {} entry points; the catalog's modules declare exactly one", module.path, entries);
    }
    const SpvReflectEntryPoint&      entry = reflected.entry_points[0];
    const std::optional<std::string> stage = StageName(entry.shader_stage);
    if (!stage.has_value()) {
        const int reported = static_cast<int>(entry.shader_stage);
        spvReflectDestroyShaderModule(&reflected);
        Fail("{} reports shader stage {}, which is not one of the reflector's stage enumerators", module.path, reported);
    }
    module.entryPoint = entry.name == nullptr ? std::string {} : entry.name;
    module.stage      = *stage;

    for (uint32_t i = 0; i < reflected.descriptor_binding_count; ++i) {
        const SpvReflectDescriptorBinding& binding = reflected.descriptor_bindings[i];
        const std::optional<std::string>   type    = DescriptorTypeName(binding.descriptor_type);
        if (!type.has_value()) {
            const int reported = static_cast<int>(binding.descriptor_type);
            spvReflectDestroyShaderModule(&reflected);
            Fail("{} declares descriptor type {}, which is not one of the reflector's descriptor-type enumerators", module.path, reported);
        }
        if (binding.name == nullptr || binding.name[0] == '\0') {
            spvReflectDestroyShaderModule(&reflected);
            Fail("{} declares an unnamed descriptor; a write has nothing to match it by", module.path);
        }
        Descriptor descriptor;
        descriptor.name    = binding.name;
        descriptor.set     = binding.set;
        descriptor.binding = binding.binding;
        descriptor.type    = *type;
        descriptor.sampler = binding.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER;
        (descriptor.sampler ? module.samplers : module.resources).push_back(std::move(descriptor));
    }
    const auto byBinding = [](const Descriptor& a, const Descriptor& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    };
    std::ranges::sort(module.resources, byBinding);
    std::ranges::sort(module.samplers, byBinding);

    for (uint32_t i = 0; i < reflected.push_constant_block_count; ++i) {
        const SpvReflectBlockVariable& block = reflected.push_constant_blocks[i];
        PushBlock                      push;
        push.paddedSize = block.padded_size;
        for (uint32_t m = 0; m < block.member_count; ++m) {
            const SpvReflectBlockVariable& member = block.members[m];
            push.members.push_back(
                Member {.name = member.name == nullptr ? std::string {} : member.name, .offset = member.offset, .size = member.size}
            );
        }
        module.pushes.push_back(std::move(push));
    }

    spvReflectDestroyShaderModule(&reflected);
    module.reflected = true;
}

/// `--bytes MACRO=path`
[[nodiscard]] auto ParseBytes(std::string_view argument) -> Module {
    const std::optional<std::pair<std::string, std::string>> fields = SplitNameValue(argument);
    if (!fields.has_value()) {
        Fail("--bytes wants MACRO=<module.spv>, got {}", argument);
    }
    Module module;
    module.macro  = fields->first;
    module.path   = fields->second;
    module.symbol = ToSymbol(module.macro);
    if (!IsIdentifier(module.symbol)) {
        Fail("--bytes {} is not a C++ identifier: the macro name is what names the byte span", module.macro);
    }
    return module;
}

/// `--set Name=Type,Type`
[[nodiscard]] auto ParseSet(std::string_view argument) -> std::pair<std::string, std::vector<std::string>> {
    const std::optional<std::pair<std::string, std::string>> fields = SplitNameValue(argument);
    if (!fields.has_value()) {
        Fail("--set wants Name=<Type>[,<Type>...], got {}", argument);
    }
    std::pair<std::string, std::vector<std::string>> set;
    set.first                  = fields->first;
    std::string_view remainder = fields->second;
    while (!remainder.empty()) {
        const size_t           comma  = remainder.find(',');
        const std::string_view member = remainder.substr(0, comma);
        if (!member.empty()) {
            set.second.emplace_back(member);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remainder.remove_prefix(comma + 1);
    }
    if (set.second.empty()) {
        Fail("--set {} names no modules", set.first);
    }
    return set;
}

// ============================================================================
// Generated text
// ============================================================================

/// The generated file, grown one formatted line at a time: std::format owns the
/// spelling of every value and `Line` owns the newline, so no emission is a chain
/// of `<<` and no `"\n"` is counted by hand.
class Emitter {
  public:
    /// A piece that is not a whole line, for the constructs that are emitted
    /// around their own members (`SlotList`, the set aliases).
    template <typename... Args>
    void Raw(std::format_string<Args...> format, Args&&... args) {
        std::format_to(std::back_inserter(_text), format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Line(std::format_string<Args...> format, Args&&... args) {
        Raw(format, std::forward<Args>(args)...);
        _text.push_back('\n');
    }

    [[nodiscard]] auto Text() const -> const std::string& {
        return _text;
    }

  private:
    std::string _text;
};

/// Which half of a module's declarations a list holds. The two are different
/// types on the renderer's side -- a sampler slot names no descriptor type,
/// because a sampler's type is not the writer's business -- so the list cannot
/// be written without saying which one it is.
enum class SlotKind { Resource, Sampler };

/// One module's binding list, as the type the write side is checked against.
[[nodiscard]] auto SlotList(const std::vector<Descriptor>& descriptors, SlotKind kind, std::string_view indent) -> std::string {
    if (descriptors.empty()) {
        return "Vk::BindingList<>";
    }
    Emitter out;
    out.Raw("Vk::BindingList<\n");
    for (size_t i = 0; i < descriptors.size(); ++i) {
        const Descriptor& descriptor = descriptors[i];
        const char* const comma      = i + 1 != descriptors.size() ? "," : "";
        if (kind == SlotKind::Sampler) {
            out.Raw("{0}    Vk::Declared::SamplerSlot<\"{1}\", {2}, {3}>{4}\n", indent, descriptor.name, descriptor.set, descriptor.binding, comma);
        } else {
            out.Raw(
                "{0}    Vk::Declared::ResourceSlot<\"{1}\", {2}, {3}, {4}>{5}\n", indent, descriptor.name, descriptor.type, descriptor.set, descriptor.binding,
                comma
            );
        }
    }
    out.Raw("{}>", indent);
    return out.Text();
}

[[nodiscard]] auto EmitHeader(const Options& options) -> std::string {
    Emitter out;
    out.Line("// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me");
    out.Line("// SPDX-License-Identifier: GPL-3.0-or-later");
    out.Line("");
    out.Line("// File: <build>/generated_shaders/ShaderBindings.hpp");
    out.Line("//");
    out.Line("// GENERATED by tools/zshader from the cooked SPIR-V. Do not edit.");
    out.Line("//");
    out.Line("// One type per module: its entry point, its stage, the bindings it declares with");
    out.Line("// their descriptor types and numbers, and the layout of its push-constant block.");
    out.Line("// One `Vk::ShaderSet<...>` alias per pass, which is what a descriptor write names.");
    out.Line("// No bytes: the modules themselves live in the generated ShaderBytecode.cpp, which");
    out.Line("// is also where these lists are held against those bytes at compile time.");
    out.Line("");
    out.Line("#pragma once");
    out.Line("");
    out.Line("#include \"Rendering.hpp\" // the RHI implementation headers");
    out.Line("#include \"pipeline/ShaderProgram.hpp\" // Vk::ShaderProgram, Vk::ShaderSet");
    out.Line("");
    out.Line("#include <cstdint>");
    out.Line("#include <span>");
    out.Line("#include <string_view>");
    out.Line("");
    out.Line("namespace ZHLN::ShaderLib {{");
    out.Line("");
    out.Line("/// One cooked module's bytes, defined in the generated ShaderBytecode.cpp --");
    out.Line("/// the only translation unit in the project that #embeds them. Defined once,");
    out.Line("/// so the image holds one copy of each module however many sites read it.");
    for (const BytesInput& input: BytesInputsOf(options)) {
        out.Line("extern const std::span<const uint8_t> {}; // {}", input.symbol, input.path);
    }
    out.Line("");
    out.Line("}} // namespace ZHLN::ShaderLib");
    out.Line("");
    out.Line("namespace ZHLN::Shaders {{");
    out.Line("");
    out.Line("/// The modules themselves. A set named after a pass (Lighting) is a different");
    out.Line("/// declaration from the module it wraps, which is why the two live in separate");
    out.Line("/// namespaces.");
    out.Line("namespace Modules {{");
    for (const auto& [type, macro]: options.catalog) {
        const Module& module = FindModule(options, type, macro);
        out.Line("");
        out.Line("struct {} {{", type);
        out.Line("    using Resources = {};", SlotList(module.resources, SlotKind::Resource, "        "));
        out.Line("    using Samplers  = {};", SlotList(module.samplers, SlotKind::Sampler, "        "));
        out.Line("");
        out.Line("    /// The entry point the module declares, and the stage it was compiled");
        out.Line("    /// for: what a pipeline is built with, read out of the module.");
        out.Line("    static constexpr const char*           EntryPoint = \"{}\";", module.entryPoint);
        out.Line("    static constexpr VkShaderStageFlagBits Stage      = {};", module.stage);
        out.Line("");
        out.Line("    /// The cooked module these declarations came from, for a hot reload and");
        out.Line("    /// for a reader that wants to compare against the file itself.");
        out.Line("    static constexpr const char* Path     = \"{}\";", AsLiteral(module.path));
        out.Line("    static constexpr uint32_t    ByteSize = {};", module.byteSize);
        if (!module.pushes.empty()) {
            const PushBlock& push = module.pushes.front();
            out.Line("");
            out.Line("    /// The push-constant block the module declares: the size a push range");
            out.Line("    /// needs and the members it is made of. The engine's own push struct is");
            out.Line("    /// written by hand (it carries VkDeviceAddress and engine math types);");
            out.Line("    /// this is what it can be held against.");
            if (module.pushes.size() > 1) {
                out.Line("    /// (The module declares {} blocks; the first is", module.pushes.size());
                out.Line("    /// what this build's pipelines take.)");
            }
            out.Line("    static constexpr uint32_t   PushSize = {};", push.paddedSize);
            out.Line("    static constexpr Vk::PushMember Push[] = {{");
            for (const Member& member: push.members) {
                out.Line("        {{\"{}\", {}, {}}},", member.name, member.offset, member.size);
            }
            out.Line("    }};");
        }
        out.Line("");
        out.Line("    [[nodiscard]] static auto Bytes() -> std::span<const uint8_t>;");
        out.Line("}};");
    }
    out.Line("");
    out.Line("}} // namespace Modules");
    out.Line("");
    out.Line("/// One set per descriptor block: the modules whose bindings that block");
    out.Line("/// serves. A write site names the set, and the set is what the compile-time");
    out.Line("/// checks read.");
    for (const auto& [name, members]: options.sets) {
        out.Raw("using {} = Vk::ShaderSet<", name);
        for (size_t i = 0; i < members.size(); ++i) {
            out.Raw("Modules::{}{}", members[i], i + 1 != members.size() ? ", " : ">;");
        }
        out.Line("");
    }
    out.Line("");
    out.Line("}} // namespace ZHLN::Shaders");
    return out.Text();
}

[[nodiscard]] auto EmitSource(const Options& options) -> std::string {
    Emitter out;
    out.Line("// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me");
    out.Line("// SPDX-License-Identifier: GPL-3.0-or-later");
    out.Line("");
    out.Line("// File: <build>/generated_shaders/ShaderBytecode.cpp");
    out.Line("//");
    out.Line("// GENERATED by tools/zshader. Do not edit.");
    out.Line("//");
    out.Line("// The only translation unit in the project that #embeds cooked SPIR-V, and the");
    out.Line("// place where ShaderBindings.hpp is held against the modules it was generated");
    out.Line("// from: every module's own bytes, walked here by SpirvBindings.hpp -- a reader");
    out.Line("// that shares nothing with the SPIRV-Reflect the tool used -- must agree with");
    out.Line("// the entry point, the stage and the binding lists the header states. A tool");
    out.Line("// that reflected a module wrongly, and a header left stale by a rebuild, both");
    out.Line("// fail to compile here instead of writing a descriptor nobody declared.");
    out.Line("");
    out.Line("#include \"ShaderBindings.hpp\"");
    out.Line("");
    out.Line("#include \"pipeline/ShaderProgram.hpp\"");
    out.Line("");
    out.Line("#include <cstdint>");
    out.Line("#include <span>");
    out.Line("");
    out.Line("namespace {{");
    out.Line("");
    for (const BytesInput& input: BytesInputsOf(options)) {
        out.Line("constexpr uint8_t {}_bytes[] = {{", input.symbol);
        out.Line("#embed \"{}\"", AsLiteral(input.path));
        out.Line("}};");
        out.Line("static_assert(sizeof({}_bytes) == {});", input.symbol, input.size);
        out.Line("");
    }
    out.Line("}} // namespace");
    out.Line("");
    out.Line("namespace ZHLN::ShaderLib {{");
    out.Line("");
    for (const BytesInput& input: BytesInputsOf(options)) {
        out.Line("const std::span<const uint8_t> {} {{{}_bytes}};", input.symbol, input.symbol);
    }
    out.Line("");
    out.Line("}} // namespace ZHLN::ShaderLib");
    out.Line("");
    out.Line("namespace ZHLN::Shaders::Modules {{");
    out.Line("");
    for (const auto& [type, macro]: options.catalog) {
        const Module& module = FindModule(options, type, macro);
        out.Line("auto {}::Bytes() -> std::span<const uint8_t> {{", type);
        out.Line("    return ZHLN::ShaderLib::{};", module.symbol);
        out.Line("}}");
        out.Line("");
    }
    out.Line("}} // namespace ZHLN::Shaders::Modules");
    out.Line("");
    for (const auto& [type, macro]: options.catalog) {
        const Module& module = FindModule(options, type, macro);
        out.Line("// {} <- {}", type, module.path);
        out.Line("static_assert(");
        out.Line("    ZHLN::Vk::ModuleMatchesBytes<ZHLN::Shaders::Modules::{}>({}_bytes),", type, module.symbol);
        out.Line("    \"ShaderBindings.hpp does not describe the module it was generated from (tools/zshader): regenerate, or fix the tool\"");
        out.Line(");");
        out.Line("");
    }
    return out.Text();
}

/// Writes `content` when the file on disk is not already that, so an unchanged
/// module leaves the generated file's mtime alone and the translation units that
/// include it are not recompiled.
void WriteIfChanged(std::string_view path, std::string_view content) {
    if (const std::optional<std::vector<uint8_t>> existing = ReadFileIfPresent(path); existing.has_value()) {
        const std::string_view current {reinterpret_cast<const char*>(existing->data()), existing->size()};
        if (current == content) {
            std::println("zshader: {} is up to date", path);
            return;
        }
    }
    std::ofstream file(std::string {path}, std::ios::binary | std::ios::trunc);
    if (!file) {
        Fail("cannot write {}", path);
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
        Fail("cannot write {}", path);
    }
    std::println("zshader: wrote {}", path);
}

} // namespace

int main(int argc, char** argv) {
    // Every name in the catalog comes from the compiler's enumerator list, so it
    // is worth knowing before anything is reflected whether this build can name
    // one: a compiler without reflection whose sources the transpiler did not
    // rewrite answers with the header's stub, and a catalog that guessed at a
    // name would be worse than no catalog -- these names are what the renderer's
    // descriptor-type and stage switches match on.
    if (!DescriptorTypeName(SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE).has_value() ||
        !StageName(SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT).has_value()) {
        Fail("this build of zshader cannot name descriptor types or stages: it needs compiler reflection (zahlen_enable_reflection) or transpiled sources");
    }

    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto             next     = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                Fail("{} wants a value", argument);
            }
            return argv[++i];
        };
        if (argument == "--out-header") {
            options.outHeader = next();
        } else if (argument == "--out-source") {
            options.outSource = next();
        } else if (argument == "--bytes") {
            Module module = ParseBytes(next());
            if (std::ranges::any_of(options.modules, [&](const Module& existing) { return existing.macro == module.macro; })) {
                Fail("--bytes {} given twice", module.macro);
            }
            Reflect(module);
            options.modules.push_back(std::move(module));
        } else if (argument == "--module") {
            const std::optional<std::pair<std::string, std::string>> fields = SplitNameValue(next());
            if (!fields.has_value()) {
                Fail("--module wants Type=<MACRO>, got {}", argv[i]);
            }
            if (!options.catalog.emplace(fields->first, fields->second).second) {
                Fail("--module {} given twice", fields->first);
            }
        } else if (argument == "--blob") {
            const std::optional<std::pair<std::string, std::string>> fields = SplitNameValue(next());
            if (!fields.has_value()) {
                Fail("--blob wants NAME=<file>, got {}", argv[i]);
            }
            BytesInput blob;
            blob.symbol = fields->first;
            blob.path   = fields->second;
            if (!IsIdentifier(blob.symbol)) {
                Fail("--blob {} is not a C++ identifier: it names the span the bytes are read through", blob.symbol);
            }
            blob.size = static_cast<uint32_t>(ReadFile(blob.path).size());
            options.blobs.push_back(std::move(blob));
        } else if (argument == "--set") {
            options.sets.push_back(ParseSet(next()));
        } else {
            Fail("unknown argument {}", argument);
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
            if (!options.catalog.contains(member)) {
                Fail("--set {} names {}, which is not a --module", name, member);
            }
        }
    }

    WriteIfChanged(options.outHeader, EmitHeader(options));
    WriteIfChanged(options.outSource, EmitSource(options));
    std::println(
        "zshader: {} module(s), {} blob(s), {} catalog type(s), {} set(s)",
        options.modules.size(), options.blobs.size(), options.catalog.size(), options.sets.size()
    );
    return 0;
}
