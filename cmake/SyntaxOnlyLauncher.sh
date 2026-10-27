#!/bin/sh
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# CMake RULE_LAUNCH_COMPILE / RULE_LAUNCH_LINK wrapper for ZHLN_SYNTAX_ONLY=ON.
# Modes:
#   SyntaxOnlyLauncher.sh compile <compiler-or-launcher> <args...>
#   SyntaxOnlyLauncher.sh skip    <compiler-or-launcher> <args...>
#   SyntaxOnlyLauncher.sh link    <linker-or-archiver>   <args...>
set -eu

if [ "$#" -lt 2 ]; then
    echo "SyntaxOnlyLauncher: expected mode and command" >&2
    exit 2
fi

mode=$1
shift

ensure_file() {
    out_dir=$(dirname "$1")
    [ -d "$out_dir" ] || mkdir -p "$out_dir"
    : > "$1"
}

case "$mode" in
    compile)
        case "$1" in
            *scan-deps*|*cmake*)
                exec "$@"
                ;;
        esac

        out=""
        has_c=0
        passthrough=0
        expect=""

        for arg in "$@"; do
            if [ "$expect" = "o" ]; then
                out=$arg
                expect=""
                continue
            elif [ "$expect" = "x" ]; then
                case "$arg" in
                    *-header|c++-module|assembler*)
                        passthrough=1
                        ;;
                esac
                expect=""
                continue
            fi

            case "$arg" in
                -o)
                    expect="o"
                    ;;
                -o*)
                    out=${arg#-o}
                    ;;
                -x)
                    expect="x"
                    ;;
                -x*-header|-xc++-module|-xassembler*)
                    passthrough=1
                    ;;
                -c)
                    has_c=1
                    ;;
                -E|-M|-MM|-S|-fdeps-format*|-emit-pch|--precompile|-fmodule-output*)
                    passthrough=1
                    ;;
                *.cppm|*.ixx|*.ccm|*.cxxm|*.c++m|*.s|*.S|*.asm|*cmake_pch*)
                    passthrough=1
                    ;;
            esac
        done

        if [ "$passthrough" -eq 1 ] || [ "$has_c" -eq 0 ] || [ -z "$out" ]; then
            exec "$@"
        fi

        if "$@" -fsyntax-only; then
            ensure_file "$out"
            exit 0
        else
            rc=$?
            rm -f "$out"
            exit "$rc"
        fi
        ;;

    skip)
        case "$1" in
            *scan-deps*|*cmake*)
                exec "$@"
                ;;
        esac

        out=""
        dep=""
        expect=""

        for arg in "$@"; do
            if [ "$expect" = "o" ]; then
                out=$arg
                expect=""
                continue
            elif [ "$expect" = "mf" ]; then
                dep=$arg
                expect=""
                continue
            fi

            case "$arg" in
                -o)
                    expect="o"
                    ;;
                -o*)
                    out=${arg#-o}
                    ;;
                -MF)
                    expect="mf"
                    ;;
                -MF*)
                    dep=${arg#-MF}
                    ;;
            esac
        done

        if [ -n "$out" ]; then
            ensure_file "$out"
        fi
        if [ -n "$dep" ] && [ -n "$out" ]; then
            dep_dir=$(dirname "$dep")
            [ -d "$dep_dir" ] || mkdir -p "$dep_dir"
            printf '%s:\n' "$out" > "$dep"
        fi
        exit 0
        ;;

    link)
        tool_base=$(basename "$1")
        case "$tool_base" in
            cmake|cmake.exe)
                exec "$@"
                ;;
            ranlib|*-ranlib|ranlib.exe|*-ranlib.exe)
                exit 0
                ;;
            ar|*-ar|ar.exe|*-ar.exe)
                if [ "$#" -ge 3 ]; then
                    ensure_file "$3"
                fi
                exit 0
                ;;
        esac

        out=""
        expect=""
        for arg in "$@"; do
            if [ "$expect" = "o" ]; then
                out=$arg
                expect=""
                continue
            fi

            case "$arg" in
                -o)
                    expect="o"
                    ;;
                -o*)
                    out=${arg#-o}
                    ;;
                -Wl,--out-implib,*)
                    implib=${arg#-Wl,--out-implib,}
                    if [ -n "$implib" ]; then
                        ensure_file "$implib"
                    fi
                    ;;
            esac
        done

        if [ -n "$out" ]; then
            ensure_file "$out"
        fi
        exit 0
        ;;

    *)
        echo "SyntaxOnlyLauncher: unknown mode '$mode'" >&2
        exit 2
        ;;
esac
