// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/main.cpp
//
// zshader: the project's only Slang compiler driver. It links libslang once
// and never invokes slangc. Every shader is compiled exactly ONCE, in-process,
// and the same linked program answers both questions the build has:
//
//   * what does the module declare? (Slang reflection + the entry point's
//     usage table -> ShaderBindings.hpp),
//   * what are its bytes? (getEntryPointCode -> .spv files and the arrays in
//     ShaderBytecode.cpp).
//
// Modes:
//
//   zshader --manifest <file>          the build's single shader step: compile,
//                                     reflect, cook .spv, emit the catalog,
//                                     the bytecode TU and the GPU host types
//   zshader compile <slangc args...>   standalone 1:1 slangc replacement
//   zshader dxc-shim <dxc args...>    Jolt's dxc command line, mapped to Slang
//   zshader --slang-module gpu_abi ... the gpu-types mode, kept for standalone
//                                     regeneration (the manifest runs it too)
//   zshader --cook-rgba ...            decode the blue-noise tile
//
// Being invoked as `dxc` (a copy/symlink of this binary), or with any dxc flag
// as the first argument, selects the dxc-shim mode automatically: Jolt runs the
// path CMake gives it with dxc's own flags and must not be patched.
//
// Exit code 0 on success, 1 with a GCC/Clang-formatted message on stderr
// otherwise. Files are only rewritten when their content changes, so an
// untouched shader does not recompile the translation units that include the
// generated header.
//
// This file is only the command line: SlangReflect.cpp compiles the modules,
// Reflect.cpp names what they declare, Emit.cpp writes the files, Manifest.cpp
// reads the batch manifest, Compile.cpp owns the standalone/dx command lines,
// and ZShader.hpp is the model the halves share.

#include "ZShader.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::ZShader {

namespace {

auto LooksLikeDxc(int argc, char** argv) -> bool {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "-T" || argument.starts_with("-T") || argument == "-Fo" || argument.starts_with("-Fo") ||
            argument == "-spirv" || argument == "-E" || argument.starts_with("-E") || argument == "-WX" ||
            argument == "-Od" || argument.starts_with("-fspv-") || argument.starts_with("-fvk-use-dx-layout")) {
            return true;
        }
    }
    return false;
}

auto Basename(std::string_view path) -> std::string_view {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

[[noreturn]] void Usage() {
    Fail(
        "usage: zshader --manifest <file> | zshader compile <slangc args...> | zshader dxc-shim <dxc args...> | "
        "zshader --slang-module <name> --slang-search <dir>... --out-gpu-types <hpp> --out-abi-spv <spv> | "
        "zshader --cook-rgba <png> --out-rgba <rgba>"
    );
}

auto ValueOf(int argc, char** argv, int& index) -> std::string_view {
    const std::string_view flag = argv[index];
    if (index + 1 >= argc) {
        Fail("{} wants a value", flag);
    }
    return argv[++index];
}

} // namespace

auto RunCommandLine(int argc, char** argv) -> int {
    // Progress lines interleave with the build system's own output, so they are
    // not left in a buffer until the process exits (zcook makes the same call
    // for the same reason).
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        Usage();
    }

    const std::string_view mode = argv[1];
    if (mode == "dxc-shim" || mode == "--dxc-compat") {
        RunDxcCompatMode(argc - 1, argv + 1);
        return 0;
    }
    if (mode == "compile") {
        RunCompileMode(argc - 1, argv + 1);
        return 0;
    }

    // Jolt's rules execute the path CMake gives them with dxc's flags: no
    // subcommand, no wrapper script. Detect that shape before dispatching.
    if (LooksLikeDxc(argc, argv) || Basename(argv[0]) == "dxc" || Basename(argv[0]) == "dxc.exe") {
        RunDxcCompatMode(argc, argv);
        return 0;
    }
    if (mode == "--manifest") {
        if (argc != 3) {
            Fail("--manifest wants exactly one manifest path");
        }
        RunManifestMode(argv[2]);
        return 0;
    }

    if (mode == "--cook-rgba" || mode == "--out-rgba") {
        std::string in;
        std::string out;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--cook-rgba") {
                in = ValueOf(argc, argv, index);
            } else if (argument == "--out-rgba") {
                out = ValueOf(argc, argv, index);
            } else {
                Fail("unknown cook-rgba argument {}", argument);
            }
        }
        if (in.empty() || out.empty()) {
            Fail("--cook-rgba and --out-rgba are both required together");
        }
        RunCookRgbaMode(in, out);
        return 0;
    }

    // The standalone gpu-types mode (the manifest command runs the same
    // implementation in-process). Kept so a developer can regenerate just the
    // ABI header without a full catalog run.
    if (mode == "--slang-module" || mode == "--out-gpu-types" || mode == "--out-abi-spv" || mode == "--slang-search") {
        GpuTypesOptions gpuTypes;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--slang-module") {
                gpuTypes.module = ValueOf(argc, argv, index);
            } else if (argument == "--slang-search") {
                gpuTypes.searchPaths.emplace_back(ValueOf(argc, argv, index));
            } else if (argument == "--out-gpu-types") {
                gpuTypes.outStructs = ValueOf(argc, argv, index);
            } else if (argument == "--out-abi-spv") {
                gpuTypes.outSpv = ValueOf(argc, argv, index);
            } else {
                Fail("unknown gpu-types argument {}", argument);
            }
        }
        if (gpuTypes.module.empty() || gpuTypes.searchPaths.empty() || gpuTypes.outStructs.empty() ||
            gpuTypes.outSpv.empty()) {
            Fail("--slang-module, --slang-search, --out-gpu-types and --out-abi-spv are all required together");
        }
        if (!NamesEnumerators()) {
            Fail(
                "this build cannot name enumerators: the compiler has no reflection and this file was not transpiled. "
                "zahlen_transpile_sources is what makes a reflection-free build work"
            );
        }
        RunGpuTypesMode(gpuTypes);
        return 0;
    }

    Usage();
}

} // namespace ZHLN::ZShader

int main(int argc, char** argv) {
    const int status = ZHLN::ZShader::RunCommandLine(argc, argv);

    if (status == 0) {
        return 0;
    }

    // RunCommandLine either returns 0 or leaves through Fail(), so a non-zero
    // return here would be a new path that forgot to report itself. Say so
    // rather than let the build step print a status and nothing else.
    std::println("zshader: RunCommandLine returned {}", status);
    std::println(stderr, "zshader: RunCommandLine returned {}", status);
    std::fflush(stdout);
    return status;
}
