// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/Manifest.cpp
//
// The batch driver. cmake/ShaderCompilation.cmake writes one declarative
// manifest (a line-oriented file, no JSON dependency in the host tool) and
// runs zshader once. This file turns it into the catalog model, compiles every
// module in-process through the shared reflection batch, and writes all of the
// build's shader artefacts from that single compile:
//
//   * <macro>.<entry>.spv          -- one per module, for hot reload/inspection
//   * ShaderBindings.hpp           -- Slang's reflection of the same link
//   * ShaderBytecode.cpp           -- the SPIR-V, inlined as C++ arrays
//   * GeneratedGpuTypes.hpp        -- the gpu_abi module's reflected layout
//   * gpu_abi.spv                  -- proof that the ABI module codegens
//
// The manifest is intentionally dumb: `key|field|field...`, one record per
// line, '#' comments. A path containing '|' is a configure error rather than a
// parse ambiguity. CMake owns the declarative table; this file owns no build
// policy beyond validating that every name it was given has a counterpart.

#include "ZShader.hpp"

#include "SlangReflect.hpp"

#include <Zahlen/Core/Ranges.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ZHLN::ZShader {

namespace {

auto SplitFields(std::string_view line, char separator) -> std::vector<std::string_view> {
    std::vector<std::string_view> fields;
    size_t                        start = 0;
    while (true) {
        const size_t end = line.find(separator, start);
        if (end == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

struct Manifest {
    Options                  options;
    std::vector<std::string> searchPaths;
    GpuTypesOptions          gpuTypes;
    bool                     hasGpuTypes = false;
};

auto ParseManifest(std::string_view path) -> Manifest {
    const std::vector<uint8_t> raw = ReadFile(path);
    const std::string          text(reinterpret_cast<const char*>(raw.data()), raw.size());
    Manifest                   manifest;
    // (macro, name, value) triples, attached after all `shader` records so
    // record order in the generated file cannot matter.
    std::vector<std::tuple<std::string, std::string, std::string>> defines;
    size_t                                                          lineNo = 0;

    size_t start = 0;
    while (start <= text.size()) {
        const size_t end  = text.find('\n', start);
        const size_t stop = end == std::string::npos ? text.size() : end;
        std::string_view line = std::string_view {text}.substr(start, stop - start);
        ++lineNo;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty() && line.front() != '#') {
            const std::vector<std::string_view> fields = SplitFields(line, '|');
            const std::string_view              key    = fields.front();
            const auto want = [&](size_t count) {
                if (fields.size() != count) {
                    Fail("{}:{}: '{}' wants {} fields, got {}", path, lineNo, key, count, fields.size());
                }
            };
            if (key == "header") {
                want(2);
                manifest.options.outHeader = fields[1];
            } else if (key == "source") {
                want(2);
                manifest.options.outSource = fields[1];
            } else if (key == "search") {
                want(2);
                manifest.searchPaths.emplace_back(fields[1]);
            } else if (key == "shader") {
                want(6);
                const std::string macro {fields[1]};
                if (macro.empty() || fields[2].empty() || fields[3].empty() || fields[4].empty() || fields[5].empty()) {
                    Fail("{}:{}: shader wants shader|MACRO|file|entry|stage|spv, none empty", path, lineNo);
                }
                if (!std::filesystem::exists(std::filesystem::path {std::string {fields[2]}})) {
                    Fail("{}:{}: shader source {} does not exist", path, lineNo, fields[2]);
                }
                ParseSlangStage(fields[2], fields[3], fields[4]); // fail early on a misspelled stage
                const std::string symbol = ToSymbol(macro);
                if (!IsIdentifier(symbol)) {
                    Fail("{}:{}: shader macro {} is not a C++ identifier", path, lineNo, macro);
                }
                const auto duplicate = ZHLN::Ranges::FindIf(
                    manifest.options.modules,
                    [&macro](const Module& existing) { return existing.macro == macro; }
                );
                if (duplicate != manifest.options.modules.end()) {
                    Fail("{}:{}: shader {} given twice", path, lineNo, macro);
                }
                manifest.options.modules.push_back(Module {
                    .macro  = macro,
                    .symbol = symbol,
                    .path   = std::string {fields[5]},
                });
                manifest.options.slangSources[macro] = SlangSource {
                    .path  = std::string {fields[2]},
                    .entry = std::string {fields[3]},
                    .stage = std::string {fields[4]},
                };
            } else if (key == "define") {
                want(4);
                defines.emplace_back(fields[1], fields[2], fields[3]);
            } else if (key == "module") {
                want(3);
                if (!manifest.options.catalog.emplace(fields[1], fields[2]).second) {
                    Fail("{}:{}: catalog type {} given twice", path, lineNo, fields[1]);
                }
            } else if (key == "set") {
                want(3);
                std::vector<std::string> members = SplitList(fields[2], ',');
                if (members.empty()) {
                    Fail("{}:{}: set {} names no modules", path, lineNo, fields[1]);
                }
                manifest.options.sets.emplace_back(std::string {fields[1]}, std::move(members));
            } else if (key == "blob") {
                want(3);
                BytesInput blob {
                    .symbol = std::string {fields[1]},
                    .path   = std::string {fields[2]},
                };
                if (!IsIdentifier(blob.symbol)) {
                    Fail("{}:{}: blob {} is not a C++ identifier", path, lineNo, blob.symbol);
                }
                blob.size            = static_cast<uint32_t>(ReadFile(blob.path).size());
                blob.hasExpectedSize = true;
                manifest.options.blobs.push_back(std::move(blob));
            } else if (key == "gpu-module") {
                want(2);
                manifest.gpuTypes.module    = fields[1];
                manifest.hasGpuTypes        = true;
            } else if (key == "gpu-header") {
                want(2);
                manifest.gpuTypes.outStructs = fields[1];
                manifest.hasGpuTypes         = true;
            } else if (key == "gpu-spv") {
                want(2);
                manifest.gpuTypes.outSpv = fields[1];
                manifest.hasGpuTypes     = true;
            } else {
                Fail("{}:{}: unknown manifest record '{}'", path, lineNo, key);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    for (const auto& [macro, name, value]: defines) {
        const auto found = manifest.options.slangSources.find(macro);
        if (found == manifest.options.slangSources.end() || found->second.path.empty()) {
            Fail("{}: define names {}, which no shader carries", path, macro);
        }
        const auto duplicate = ZHLN::Ranges::FindIf(found->second.defines, [&name](const auto& existing) {
            return existing.first == name;
        });
        if (duplicate != found->second.defines.end()) {
            Fail("{}: define {}={} given twice", path, macro, name);
        }
        found->second.defines.emplace_back(name, value);
    }
    return manifest;
}

void Validate(const Manifest& manifest, std::string_view path) {
    const Options& options = manifest.options;
    if (options.outHeader.empty() || options.outSource.empty()) {
        Fail("{}: header and source outputs are both required", path);
    }
    if (options.modules.empty()) {
        Fail("{}: no shaders in the manifest", path);
    }
    if (manifest.searchPaths.empty()) {
        Fail("{}: no search paths; the modules will not resolve their imports", path);
    }
    for (const auto& [macro, source]: options.slangSources) {
        if (source.path.empty()) {
            Fail("{}: slang source {} has no shader record", path, macro);
        }
    }
    for (const Module& module: options.modules) {
        if (options.slangSources.find(module.macro) == options.slangSources.end()) {
            Fail("{}: shader {} has no compile record", path, module.macro);
        }
    }
    for (const auto& [type, macro]: options.catalog) {
        const auto known = ZHLN::Ranges::FindIf(
            options.modules, [&macro](const Module& module) { return module.macro == macro; }
        );
        if (known == options.modules.end()) {
            Fail("{}: catalog type {} names {}, which no shader defines", path, type, macro);
        }
    }
    for (const auto& [name, members]: options.sets) {
        for (const std::string& member: members) {
            if (options.catalog.find(member) == options.catalog.end()) {
                Fail("{}: set {} names {}, which is not a catalog type", path, name, member);
            }
        }
    }
    if (manifest.hasGpuTypes &&
        (manifest.gpuTypes.module.empty() || manifest.gpuTypes.outStructs.empty() || manifest.gpuTypes.outSpv.empty())) {
        Fail("{}: gpu-module, gpu-header and gpu-spv are all required together", path);
    }
}

} // namespace

void RunManifestMode(std::string_view manifestPath) {
    Manifest manifest = ParseManifest(manifestPath);
    Validate(manifest, manifestPath);

    if (!NamesEnumerators()) {
        Fail(
            "this build cannot name enumerators: the compiler has no reflection and this file was not transpiled. "
            "zahlen_transpile_sources is what makes a reflection-free build work"
        );
    }

    // ONE compile per module: reflection and SPIR-V from the same linked
    // program, shared sessions/parsed modules across the worker threads.
    ReflectAllModules(manifest.options.modules, manifest.options.slangSources, manifest.searchPaths);

    // The .spv files are a convenience (hot reload watches these paths, and a
    // human can inspect one); the bytes in the binary come from the arrays
    // EmitSource writes, not from these files.
    for (const Module& module: manifest.options.modules) {
        WriteFileIfChanged(
            module.path,
            std::string_view {reinterpret_cast<const char*>(module.spirv.data()), module.spirv.size()}
        );
    }

    WriteFileIfChanged(manifest.options.outHeader, EmitHeader(manifest.options));
    WriteFileIfChanged(manifest.options.outSource, EmitSource(manifest.options));

    if (manifest.hasGpuTypes) {
        manifest.gpuTypes.searchPaths = manifest.searchPaths;
        RunGpuTypesMode(manifest.gpuTypes);
    }

    std::println(
        "zshader: {} module(s), {} blob(s), {} catalog type(s), {} set(s), 1 compile per module",
        manifest.options.modules.size(), manifest.options.blobs.size(), manifest.options.catalog.size(),
        manifest.options.sets.size()
    );
}

} // namespace ZHLN::ZShader
