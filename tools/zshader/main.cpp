// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zshader/main.cpp
//
// zshader: reflects cooked SPIR-V into the renderer's shader catalog.
//
// Runs after the shader cooks, only in a build in which a module changed, and
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
//     embeds it (src/render/GpuAbi.hpp, where that check lives).
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
//
// This file is only the command line: Reflect.cpp reads the modules, Emit.cpp
// writes the files, and ZShader.hpp is the model the halves share.

#include "ZShader.hpp"

#include <Zahlen/Core/Ranges.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ZShader {

namespace {

// `--bytes MACRO=path`: the module is named by the macro the build knows it by,
// and read under the symbol that macro becomes.
auto ParseBytes(std::string_view argument) -> Module {
    const auto [macro, path] = SplitOnce(argument, '=');
    if (macro.empty() || path.empty()) {
        Fail("--bytes wants MACRO=<module.spv>, got {}", argument);
    }
    Module module;
    module.macro  = std::string {macro};
    module.path   = std::string {path};
    module.symbol = ToSymbol(module.macro);
    if (!IsIdentifier(module.symbol)) {
        Fail("--bytes {} is not a C++ identifier: the macro name is what names the byte span", module.macro);
    }
    return module;
}

// `--module Type=MACRO`: one catalog type and the cooked module it wraps.
auto ParseModule(std::string_view argument) -> std::pair<std::string, std::string> {
    const auto [type, macro] = SplitOnce(argument, '=');
    if (type.empty() || macro.empty()) {
        Fail("--module wants Type=<MACRO>, got {}", argument);
    }
    return {std::string {type}, std::string {macro}};
}

// `--blob Name=file`: bytes that are not a shader, sized here so the generated
// file can assert the size the image actually holds.
auto ParseBlob(std::string_view argument) -> BytesInput {
    const auto [name, path] = SplitOnce(argument, '=');
    if (name.empty() || path.empty()) {
        Fail("--blob wants NAME=<file>, got {}", argument);
    }
    BytesInput blob {
        .symbol = std::string {name},
        .path   = std::string {path},
    };
    if (!IsIdentifier(blob.symbol)) {
        Fail("--blob {} is not a C++ identifier: it names the span the bytes are read through", blob.symbol);
    }
    blob.size = static_cast<uint32_t>(ReadFile(blob.path).size());
    return blob;
}

// `--set Name=Type,Type`: one descriptor block and the modules whose bindings
// it serves.
auto ParseSet(std::string_view argument) -> std::pair<std::string, std::vector<std::string>> {
    const auto [name, list] = SplitOnce(argument, '=');
    if (name.empty() || list.empty()) {
        Fail("--set wants Name=<Type>[,<Type>...], got {}", argument);
    }
    std::vector<std::string> members = SplitList(list, ',');
    if (members.empty()) {
        Fail("--set {} names no modules", name);
    }
    return {std::string {name}, std::move(members)};
}

// Writes `content` to `path`, unless the file already says exactly that: the
// generated files are inputs to the build, so rewriting an identical one would
// recompile every translation unit that includes it.
auto WriteIfChanged(const std::string& path, const std::string& content) -> bool {
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            const std::string current {std::istreambuf_iterator<char>(existing), std::istreambuf_iterator<char>()};
            if (current == content) {
                std::println("zshader: {} is up to date", path);
                return false;
            }
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Fail("cannot write {}", path);
    }
    file << content;
    if (!file) {
        Fail("cannot write {}", path);
    }
    std::println("zshader: wrote {}", path);
    return true;
}

} // namespace

auto RunCommandLine(int argc, char** argv) -> int {
    // Progress lines interleave with the build system's own output, so they are
    // not left in a buffer until the process exits (zcook makes the same call
    // for the same reason).
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Before reading a module or writing a file: a build compiled against the
    // reflection stand-ins cannot name a single enumerator, so it would write a
    // catalog whose every descriptor type is the sentinel. One sentence about
    // the build beats one lie per module.
    if (!NamesEnumerators()) {
        Fail(
            "this build cannot name enumerators: the compiler has no reflection and this file was not transpiled. "
            "zahlen_transpile_sources in cmake/ShaderCompilation.cmake is what makes a reflection-free build work"
        );
    }

    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto             next     = [&index, argc, argv, argument]() -> std::string_view {
            if (index + 1 >= argc) {
                Fail("{} wants a value", argument);
            }
            return argv[++index];
        };
        if (argument == "--out-header") {
            options.outHeader = next();
        } else if (argument == "--out-source") {
            options.outSource = next();
        } else if (argument == "--bytes") {
            Module     module    = ParseBytes(next());
            const auto duplicate = ZHLN::Ranges::FindIf(options.modules, [&module](const Module& existing) { return existing.macro == module.macro; });
            if (duplicate != options.modules.end()) {
                Fail("--bytes {} given twice", module.macro);
            }
            ReflectModule(module);
            options.modules.push_back(std::move(module));
        } else if (argument == "--module") {
            const auto [type, macro] = ParseModule(next());
            if (!options.catalog.emplace(type, macro).second) {
                Fail("--module {} given twice", type);
            }
        } else if (argument == "--blob") {
            options.blobs.push_back(ParseBlob(next()));
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
            if (options.catalog.find(member) == options.catalog.end()) {
                Fail("--set {} names {}, which is not a --module", name, member);
            }
        }
    }

    WriteIfChanged(options.outHeader, EmitHeader(options));
    WriteIfChanged(options.outSource, EmitSource(options));
    std::println(
        "zshader: {} module(s), {} blob(s), {} catalog type(s), {} set(s)", options.modules.size(), options.blobs.size(), options.catalog.size(), options.sets.size()
    );
    return 0;
}

} // namespace ZHLN::ZShader

int main(int argc, char** argv) {
    return ZHLN::ZShader::RunCommandLine(argc, argv);
}
