// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zshader/main.cpp
//
// zshader: compiles Slang into the renderer's shader catalog.
//
// Runs after the shader cooks, only in a build in which a module changed, and
// writes two files:
//
//   * ShaderBindings.hpp -- pure text. One type per module (its entry point, its
//     stage, its binding lists with descriptor types, sets and binding numbers,
//     its push-constant layout) and one `Vk::ShaderSet<...>` alias per pass.
//     Nothing in it is bytes: the lists come from Slang's own reflection and
//     entry-point metadata, never from a decode of the modules;
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
// The tool is deliberately the second opinion, not the only one: it compiles
// the catalog's modules from Slang in-process, and the assertion in the
// generated source -- which reads the cooked bytes -- is what keeps those
// compiles honest. The size in `module.byteSize` is the only fact the tool
// takes from the cooked file itself.
//
// Usage (the catalog):
//
//   zshader --out-header <path> --out-source <path>
//           --bytes <MACRO>=<module.spv> ...            (every cooked module)
//           --slang-source <MACRO>=<path>,<Entry>,<stage> ... (how each compiles)
//           --slang-define <MACRO>=<NAME>[=<VALUE>] ... (per-module -D flags)
//           --slang-search <dir> ...                   (Slang import search dirs)
//           --module <Type>=<MACRO> ...                 (the catalog's modules)
//           --blob <Name>=<file> ...                    (bytes that are not shaders)
//           --set <Set>=<Type>,<Type> ...               (one alias per pass)
//
// Usage (the gpu types, a separate mode with a separate output):
//
//   zshader --slang-module <gpu_abi> --slang-search <dir> ...
//           --out-gpu-types <GeneratedGpuTypes.hpp> --out-abi-spv <gpu_abi.spv>
//
// --slang-search is the one flag the modes share. The two modes never mix in
// one invocation.
//
// Exit code 0 on success, 1 with a message on stderr otherwise. Files are only
// rewritten when their content changes, so an untouched shader does not
// recompile the translation units that include the header.
//
// This file is only the command line: SlangReflect.cpp compiles the modules,
// Reflect.cpp names what they declare, Emit.cpp writes the files, and
// ZShader.hpp is the model the halves share.

#include "ZShader.hpp"

#include "SlangReflect.hpp"

#include <Zahlen/Core/Ranges.hpp>

#include <cstdio>
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

// `--slang-source MACRO=path,Entry,stage`: how to compile the module the macro
// names. The stage is checked now, so a misspelled stage fails before any
// module compiles rather than twenty modules in.
auto ParseSlangSource(std::string_view argument) -> std::pair<std::string, SlangSource> {
    const auto [macro, rest] = SplitOnce(argument, '=');
    const std::vector<std::string> fields = SplitList(rest, ',');
    if (macro.empty() || fields.size() != 3 || fields[0].empty() || fields[1].empty() || fields[2].empty()) {
        Fail("--slang-source wants MACRO=<path>,<Entry>,<stage>, got {}", argument);
    }
    ParseSlangStage(fields[0], fields[1], fields[2]);
    SlangSource source{
        .path  = fields[0],
        .entry = fields[1],
        .stage = fields[2],
    };
    return {std::string {macro}, std::move(source)};
}

// `--slang-define MACRO=NAME[=VALUE]`: one -D flag for the module's compile. A
// bare NAME arrives with value "1": defined()-ness is all any shader asks.
struct SlangDefine {
    std::string macro;
    std::string name;
    std::string value;
};

auto ParseSlangDefine(std::string_view argument) -> SlangDefine {
    const auto [macro, rest] = SplitOnce(argument, '=');
    if (macro.empty() || rest.empty()) {
        Fail("--slang-define wants MACRO=<NAME>[=<VALUE>], got {}", argument);
    }
    // SplitOnce reports a missing separator as two empty views, which is also
    // what `NAME=` splits to -- the bare NAME is the one where both are empty.
    const auto [name, value] = SplitOnce(rest, '=');
    const bool         bare  = name.empty() && value.empty();
    const std::string_view define = bare ? rest : name;
    if (define.empty()) {
        Fail("--slang-define wants MACRO=<NAME>[=<VALUE>], got {}", argument);
    }
    return SlangDefine{
        .macro = std::string {macro},
        .name  = std::string {define},
        .value = value.empty() ? "1" : std::string {value},
    };
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

    Options                  options;
    GpuTypesOptions          gpuTypes;
    std::vector<std::string> slangSearch; // the one flag the modes share: --slang-search fills it for both
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
        } else if (argument == "--out-gpu-types") {
            gpuTypes.outStructs = next();
        } else if (argument == "--slang-module") {
            gpuTypes.module = next();
        } else if (argument == "--slang-search") {
            slangSearch.emplace_back(next());
        } else if (argument == "--out-abi-spv") {
            gpuTypes.outSpv = next();
        } else if (argument == "--bytes") {
            Module     module    = ParseBytes(next());
            const auto duplicate = ZHLN::Ranges::FindIf(options.modules, [&module](const Module& existing) { return existing.macro == module.macro; });
            if (duplicate != options.modules.end()) {
                Fail("--bytes {} given twice", module.macro);
            }
            options.modules.push_back(std::move(module));
        } else if (argument == "--slang-source") {
            auto [macro, source] = ParseSlangSource(next());
            auto [stored, inserted] = options.slangSources.try_emplace(macro, std::move(source));
            if (!inserted) {
                // A --slang-define that arrived first holds the key with an
                // empty path: the source fills the placeholder it left, and
                // the flag order stops mattering. Anything else is the source
                // twice.
                if (!stored->second.path.empty()) {
                    Fail("--slang-source {} given twice", macro);
                }
                stored->second.path  = source.path;
                stored->second.entry = source.entry;
                stored->second.stage = source.stage;
            }
        } else if (argument == "--slang-define") {
            SlangDefine                                    define  = ParseSlangDefine(next());
            std::vector<std::pair<std::string, std::string>>& defines = options.slangSources[define.macro].defines;
            const auto duplicate = ZHLN::Ranges::FindIf(defines, [&define](const std::pair<std::string, std::string>& existing) { return existing.first == define.name; });
            if (duplicate != defines.end()) {
                Fail("--slang-define {}={} given twice", define.macro, define.name);
            }
            defines.emplace_back(std::move(define.name), std::move(define.value));
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

    const bool wantsGpuTypes = !gpuTypes.outStructs.empty() || !gpuTypes.module.empty() || !gpuTypes.outSpv.empty();
    if (wantsGpuTypes) {
        if (gpuTypes.outStructs.empty() || gpuTypes.module.empty() || slangSearch.empty() || gpuTypes.outSpv.empty()) {
            Fail("--slang-module, --slang-search, --out-gpu-types and --out-abi-spv are all required together");
        }
        if (!options.outHeader.empty() || !options.outSource.empty() || !options.modules.empty() || !options.catalog.empty() ||
            !options.blobs.empty() || !options.sets.empty() || !options.slangSources.empty()) {
            Fail("the gpu-types mode takes no catalog arguments; run the catalog separately");
        }
        gpuTypes.searchPaths = slangSearch;
        RunGpuTypesMode(gpuTypes);
        return 0;
    }

    if (options.outHeader.empty() || options.outSource.empty()) {
        Fail("--out-header and --out-source are both required");
    }
    if (options.modules.empty()) {
        Fail("no --bytes inputs: there is nothing to reflect");
    }
    if (slangSearch.empty()) {
        Fail("no --slang-search search paths; the modules will not resolve their imports");
    }
    for (const auto& [macro, source]: options.slangSources) {
        if (source.path.empty()) {
            Fail("--slang-define names {}, which no --slang-source carries", macro);
        }
        const auto known =
            ZHLN::Ranges::FindIf(options.modules, [&macro](const Module& module) { return module.macro == macro; });
        if (known == options.modules.end()) {
            Fail("--slang-source {} names no --bytes module", macro);
        }
    }
    for (Module& module: options.modules) {
        const auto found = options.slangSources.find(module.macro);
        if (found == options.slangSources.end()) {
            Fail("--bytes {} has no --slang-source; the catalog compiles every module from source", module.macro);
        }
        ReflectModule(module, found->second, slangSearch);
    }
    for (const auto& [name, members]: options.sets) {
        for (const std::string& member: members) {
            if (options.catalog.find(member) == options.catalog.end()) {
                Fail("--set {} names {}, which is not a --module", name, member);
            }
        }
    }

    WriteFileIfChanged(options.outHeader, EmitHeader(options));
    WriteFileIfChanged(options.outSource, EmitSource(options));
    std::println(
        "zshader: {} module(s), {} blob(s), {} catalog type(s), {} set(s)", options.modules.size(), options.blobs.size(), options.catalog.size(), options.sets.size()
    );
    return 0;
}

} // namespace ZHLN::ZShader

int main(int argc, char** argv) {
    return ZHLN::ZShader::RunCommandLine(argc, argv);
}
