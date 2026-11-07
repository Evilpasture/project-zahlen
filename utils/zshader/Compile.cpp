// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/Compile.cpp
//
// The two standalone compiler front doors, both backed by libslang's own
// command-line parser (the same one slangc uses):
//
//   zshader compile <slangc args...>   1:1 replacement for slangc for the dev
//                                     checks that used to shell out to it
//   zshader dxc-shim <dxc args...>    Jolt's dxc command line, mapped to the
//                                     libslang options that reproduce its
//                                     SPIR-V byte for byte. No bash, no PATH
//                                     lookup, no second compiler binary.
//
// Both end in one libslang compile request; the SPIR-V comes straight out of
// getEntryPointCode and is written by this tool. Diagnostics are rewritten to
// file:line:col: error/warning so Ninja and the IDE see them like any other
// C++ diagnostic.

#include "ZShader.hpp"

#include <slang-com-ptr.h>
#include <slang-deprecated.h> // spCreateCompileRequest / spProcessCommandLineArguments
#include <slang.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::ZShader {

namespace {

struct Invocation {
    std::vector<std::string> args;   // slangc-style argv, no program name
    std::string              output; // -o / -Fo
    std::string              source; // for diagnostics and the summary
};

void RunSlangcArgs(const Invocation& invocation) {
    if (invocation.output.empty()) {
        Fail("no output file given (-o / -Fo)");
    }
    if (invocation.args.empty()) {
        Fail("no compiler arguments given");
    }

    Slang::ComPtr<slang::IGlobalSession> global;
    if (slang::createGlobalSession(global.writeRef()) != SLANG_OK) {
        Fail("cannot create the Slang global session");
    }
    SlangCompileRequest* request = spCreateCompileRequest(global.get());
    if (request == nullptr) {
        Fail("cannot create a Slang compile request");
    }

    std::vector<const char*> argv;
    argv.reserve(invocation.args.size());
    for (const std::string& argument: invocation.args) {
        argv.push_back(argument.c_str());
    }

    SlangResult parsed = spProcessCommandLineArguments(request, argv.data(), static_cast<int>(argv.size()));
    const char* parseDiag = spGetDiagnosticOutput(request);
    if (SLANG_FAILED(parsed)) {
        const std::string text = parseDiag != nullptr ? parseDiag : "";
        spDestroyCompileRequest(request);
        Fail(
            "cannot parse the compiler arguments for '{}':\n{}", invocation.source,
            text.empty() ? std::string {"no diagnostic"} : FormatSlangDiagnostics(text, invocation.source)
        );
    }

    const SlangResult result = spCompile(request);
    const char*       rawDiag = spGetDiagnosticOutput(request);
    const std::string diag    = rawDiag != nullptr ? rawDiag : "";
    // A failed compile has no entry point to ask for code from: asking anyway
    // dereferences a null program inside libslang. Check the result FIRST.
    if (SLANG_FAILED(result)) {
        spDestroyCompileRequest(request);
        Fail(
            "'{}': Slang cannot emit SPIR-V:\n{}", invocation.source,
            diag.empty() ? std::string {"no diagnostic"} : FormatSlangDiagnostics(diag, invocation.source)
        );
    }
    size_t            size  = 0;
    const void* const code  = spGetEntryPointCode(request, 0, &size);
    const auto* const bytes = static_cast<const uint8_t*>(code);
    if (code == nullptr || size < 4) {
        spDestroyCompileRequest(request);
        Fail(
            "'{}': Slang emitted no SPIR-V:\n{}", invocation.source,
            diag.empty() ? std::string {"no diagnostic"} : FormatSlangDiagnostics(diag, invocation.source)
        );
    }
    if (!diag.empty()) {
        std::print(stderr, "{}", FormatSlangDiagnostics(diag, invocation.source));
    }
    if (size % sizeof(uint32_t) != 0 ||
        !(bytes[0] == 0x03 && bytes[1] == 0x02 && bytes[2] == 0x23 && bytes[3] == 0x07)) {
        spDestroyCompileRequest(request);
        Fail("'{}': Slang emitted {} bytes that are not SPIR-V", invocation.source, size);
    }

    WriteFileIfChanged(invocation.output, std::string_view {reinterpret_cast<const char*>(bytes), size});
    spDestroyCompileRequest(request);
}

auto StageOf(std::string_view profile) -> std::string {
    if (profile.starts_with("cs_")) {
        return "compute";
    }
    if (profile.starts_with("ps_")) {
        return "fragment";
    }
    if (profile.starts_with("vs_")) {
        return "vertex";
    }
    if (profile.starts_with("gs_")) {
        return "geometry";
    }
    if (profile.starts_with("hs_")) {
        return "hull";
    }
    if (profile.starts_with("ds_")) {
        return "domain";
    }
    if (profile.starts_with("ms_")) {
        return "mesh";
    }
    if (profile.starts_with("as_")) {
        return "amplification";
    }
    Fail("unsupported shader profile '{}'", profile);
}

} // namespace

void RunCompileMode(int argc, char** argv) {
    // argv[0] is the "compile" subcommand itself; everything after it is a
    // slangc command line. Only -o and the input file are picked out here; the
    // rest belong to libslang's parser verbatim.
    Invocation invocation;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "-o" || argument == "--output") {
            if (index + 1 >= argc) {
                Fail("{} wants a path", argument);
            }
            invocation.output = argv[++index];
            invocation.args.emplace_back("-o");
            invocation.args.emplace_back(invocation.output);
            continue;
        }
        if (argument.starts_with("-o") && argument.size() > 2) {
            invocation.output = std::string {argument.substr(2)};
            invocation.args.emplace_back(argument);
            continue;
        }
        if (!argument.starts_with("-") && invocation.source.empty()) {
            invocation.source = argument;
        }
        invocation.args.emplace_back(argument);
    }
    if (invocation.source.empty()) {
        Fail("compile: no shader source given");
    }
    if (invocation.output.empty()) {
        Fail("compile: no output file given (-o)");
    }
    RunSlangcArgs(invocation);
}

void RunDxcCompatMode(int argc, char** argv) {
    // argv[0] is "dxc-shim" (explicit) or this executable (auto-detected);
    // args from index 1 are dxc's. The mapping reproduces the SPIR-V Jolt's
    // old shell shim produced, byte for byte (verified against all 17 shaders).
    std::string              entry = "main";
    std::string              stage;
    std::string              output;
    std::string              source;
    std::vector<std::string> slangArgs; // dxc flags with a Slang equivalent
    const std::string        profile = "spirv_1_3";

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "-E") {
            if (index + 1 >= argc) {
                Fail("-E needs an entry point");
            }
            entry = argv[++index];
        } else if (argument.starts_with("-E") && argument.size() > 2) {
            entry = std::string {argument.substr(2)};
        } else if (argument == "-T") {
            if (index + 1 >= argc) {
                Fail("-T needs a shader profile");
            }
            stage = StageOf(argv[++index]);
        } else if (argument.starts_with("-T") && argument.size() > 2) {
            stage = StageOf(argument.substr(2));
        } else if (argument == "-Fo") {
            if (index + 1 >= argc) {
                Fail("-Fo needs a path");
            }
            output = argv[++index];
        } else if (argument.starts_with("-Fo") && argument.size() > 3) {
            output = std::string {argument.substr(3)};
        } else if (argument == "-I") {
            if (index + 1 >= argc) {
                Fail("-I needs a directory");
            }
            slangArgs.emplace_back("-I");
            slangArgs.emplace_back(argv[++index]);
        } else if (argument.starts_with("-I") && argument.size() > 2) {
            slangArgs.emplace_back(argument);
        } else if (argument == "-WX") {
            // -warnings-disable last: it wins over -warnings-as-errors all.
            // 30081 is "implicit conversion from 'uint' to 'bool'", legal HLSL
            // that only Slang's style check objects to (HairIntegrate.h).
            slangArgs.emplace_back("-warnings-as-errors");
            slangArgs.emplace_back("all");
            slangArgs.emplace_back("-warnings-disable");
            slangArgs.emplace_back("30081");
        } else if (argument == "-Od") {
            slangArgs.emplace_back("-O0");
        } else if (argument == "-O0" || argument == "-O1" || argument == "-O2" || argument == "-O3") {
            slangArgs.emplace_back(argument);
        } else if (argument.starts_with("-fspv-debug=")) {
            slangArgs.emplace_back("-g3");
        } else if (argument == "-fvk-use-dx-layout") {
            slangArgs.emplace_back(argument);
        } else if (argument == "-spirv" || argument == "-all_resources_bound" || argument == "-Zi" ||
                   argument == "-Qembed_debug" || argument == "-Qstrip_debug" || argument == "-Qstrip_reflect" ||
                   argument.starts_with("-fspv-entrypoint-name=") || argument.starts_with("-fspv-target-env=") ||
                   argument.starts_with("-fvk-binding-shift=")) {
            // Vulkan needs none of these; dxc's defaults differ from Slang's.
        } else if (argument.starts_with("-")) {
            std::println(stderr, "zshader dxc-shim: dropping unsupported flag {}", argument);
        } else if (source.empty()) {
            source = argument;
        } else {
            Fail("dxc-shim: expected one shader source, got a second one ({})", argument);
        }
    }

    if (source.empty()) {
        Fail("dxc-shim: no shader source given");
    }
    if (output.empty()) {
        Fail("dxc-shim: no output file given (-Fo)");
    }
    if (stage.empty()) {
        Fail("dxc-shim: no shader stage given (-T)");
    }

    Invocation invocation {
        .output = output,
        .source = source,
    };
    invocation.args = {
        "-entry", entry, "-stage", stage, "-target", "spirv", "-profile", profile,
    };
    invocation.args.insert(invocation.args.end(), slangArgs.begin(), slangArgs.end());
    invocation.args.emplace_back("-o");
    invocation.args.emplace_back(output);
    invocation.args.emplace_back(source);
    RunSlangcArgs(invocation);
}

} // namespace ZHLN::ZShader
