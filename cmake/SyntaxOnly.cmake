# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/SyntaxOnly.cmake
#
# Configures Ninja/Makefile targets to run the C/C++ frontend in -fsyntax-only
# mode without emitting object code or invoking the linker/archiver.
#
# Build-time host tools (zshader, LuaJIT's minilua/buildvm, vendored slangc)
# continue to compile and link normally so generated headers (ShaderBindings.hpp,
# GeneratedGpuTypes.hpp, luajit.h) exist before dependent translation units are
# checked. Precompiled headers and C++20 module BMIs (.pcm/.gcm) also pass
# through untouched so downstream translation units parse against real BMIs/PCHs.

set(_zhln_syntax_launcher "${CMAKE_CURRENT_LIST_DIR}/SyntaxOnlyLauncher.sh")

if(WIN32)
    find_program(ZHLN_POSIX_SH NAMES sh bash REQUIRED)
    set(_zhln_syntax_prefix "\"${ZHLN_POSIX_SH}\" \"${_zhln_syntax_launcher}\"")
else()
    set(_zhln_syntax_prefix "\"${_zhln_syntax_launcher}\"")
endif()

function(_zhln_collect_directories out_var root_dir)
    set(collected "${root_dir}")
    get_property(subdirs DIRECTORY "${root_dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        _zhln_collect_directories(sub_collected "${sub}")
        list(APPEND collected ${sub_collected})
    endforeach()
    set(${out_var} "${collected}" PARENT_SCOPE)
endfunction()

function(zahlen_apply_syntax_only)
    _zhln_collect_directories(_all_dirs "${PROJECT_SOURCE_DIR}")
    list(REMOVE_DUPLICATES _all_dirs)

    set(_extern_prefix "${PROJECT_SOURCE_DIR}/extern/")
    set(_luajit_prefix "${PROJECT_SOURCE_DIR}/extern/LuaJIT")
    set(_slang_prefix  "${PROJECT_SOURCE_DIR}/extern/slang")
    set(_zshader_dir   "${PROJECT_SOURCE_DIR}/tools/zshader")

    foreach(_dir IN LISTS _all_dirs)
        get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_tgt IN LISTS _targets)
            get_target_property(_type ${_tgt} TYPE)
            if(NOT _type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
                continue()
            endif()

            get_target_property(_src_dir ${_tgt} SOURCE_DIR)

            # Build-time shader reflection tool must compile and link for real.
            if(_tgt STREQUAL "zshader" OR _src_dir STREQUAL _zshader_dir)
                continue()
            endif()

            string(FIND "${_src_dir}/" "${_extern_prefix}" _extern_pos)
            if(_extern_pos EQUAL 0)
                # Leave third-party host codegen executables and LuaJIT/Slang
                # toolchains intact; skip heavy pure-library vendor compiles.
                string(FIND "${_src_dir}" "${_luajit_prefix}" _luajit_pos)
                string(FIND "${_src_dir}" "${_slang_prefix}" _slang_pos)
                if(_type STREQUAL "EXECUTABLE" OR _luajit_pos EQUAL 0 OR _slang_pos EQUAL 0)
                    continue()
                endif()

                set_property(TARGET ${_tgt} PROPERTY RULE_LAUNCH_COMPILE "${_zhln_syntax_prefix} skip")
                if(NOT _type STREQUAL "OBJECT_LIBRARY")
                    set_property(TARGET ${_tgt} PROPERTY RULE_LAUNCH_LINK "${_zhln_syntax_prefix} link")
                endif()
            else()
                set_property(TARGET ${_tgt} PROPERTY RULE_LAUNCH_COMPILE "${_zhln_syntax_prefix} compile")
                if(NOT _type STREQUAL "OBJECT_LIBRARY")
                    set_property(TARGET ${_tgt} PROPERTY RULE_LAUNCH_LINK "${_zhln_syntax_prefix} link")
                endif()
            endif()
        endforeach()
    endforeach()
endfunction()

zahlen_apply_syntax_only()
