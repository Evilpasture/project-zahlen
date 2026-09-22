# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/FennelCompiler.cmake
#
# --- FENNEL COMPILER INTEGRATION & EXPORTED HELPERS ---
# Prefer the vendored Fennel library (extras/Scripting/Lua/scripts/core/fennel.lua)
# driven by LuaJIT. A host `fennel` on PATH is not required. -DFENNEL_BIN= still
# overrides if someone wants a system compiler.
#
# The numbers of the setup are captured here, once, and the exported
# zahlen_compile_fennel() turns .fnnel sources into the .lua files the runtime
# loads from the build tree.

set(ZHLN_FENNEL_LUA
    "${CMAKE_SOURCE_DIR}/extras/Scripting/Lua/scripts/core/fennel.lua")
set(ZHLN_FENNELC "${CMAKE_SOURCE_DIR}/tools/fennelc.lua")
set(ZHLN_FENNEL_DEPENDS "")

if(FENNEL_BIN)
    set(ZHLN_FENNEL_USES_VENDOR OFF)
elseif(EXISTS "${ZHLN_FENNEL_LUA}")
    if(TARGET luajit)
        set(ZHLN_LUA_FOR_FENNEL "$<TARGET_FILE:luajit>")
        set(ZHLN_FENNEL_DEPENDS luajit)
    else()
        find_program(ZHLN_LUA_FOR_FENNEL NAMES luajit luajit-2.1 lua)
    endif()
    if(NOT ZHLN_LUA_FOR_FENNEL)
        message(FATAL_ERROR
            "Need LuaJIT (or lua) to run the vendored Fennel compiler at "
            "${ZHLN_FENNEL_LUA}. Init extern/LuaJIT or put luajit on PATH.")
    endif()
    set(ZHLN_FENNEL_USES_VENDOR ON)
    message(STATUS "Using vendored Fennel compiler: ${ZHLN_FENNEL_LUA}")
else()
    message(FATAL_ERROR
        "Vendored Fennel compiler not found at ${ZHLN_FENNEL_LUA}. "
        "Pass -DFENNEL_BIN=/path/to/fennel if you must use a host compiler.")
endif()

function(zahlen_compile_fennel TARGET_NAME OUTPUT_LUA_FILES_VAR)
    set(LUA_OUTPUTS "")
    foreach(FNL_SRC IN LISTS ARGN)
        get_filename_component(FILE_NAME_WE ${FNL_SRC} NAME_WLE)
        get_filename_component(FILE_DIR ${FNL_SRC} DIRECTORY)

        # The runtime resolves a module as `scripts/core/<name>.lua` relative to
        # ZHLN_COMPILED_SCRIPTS_DIR, which is the build tree, so the build has to
        # mirror the source path from its `scripts` component onwards rather than
        # from the repository root. While these sources lived at <root>/scripts
        # the two readings agreed; now that they live under extras/Scripting/Lua,
        # mirroring the whole path puts the compiled modules in
        # build/extras/Scripting/Lua/scripts/, which is on no package.path, and
        # every `require 'scripts.core.*'` misses. The match is anchored at a
        # path boundary, so a directory that merely ends in the letters --
        # myscripts/ -- is not one, and a source tree with no `scripts` component
        # keeps mirroring its full path, as before.
        file(RELATIVE_PATH REL_PATH "${CMAKE_SOURCE_DIR}" "${FILE_DIR}")
        string(REGEX REPLACE "(^|.*/)scripts(/|$)" "scripts\\2" REL_PATH "${REL_PATH}")
        set(OUT_DIR "${CMAKE_BINARY_DIR}/${REL_PATH}")
        file(MAKE_DIRECTORY "${OUT_DIR}")

        set(OUTPUT_LUA "${OUT_DIR}/${FILE_NAME_WE}.lua")

        if(ZHLN_FENNEL_USES_VENDOR)
            add_custom_command(
                OUTPUT ${OUTPUT_LUA}
                COMMAND ${ZHLN_LUA_FOR_FENNEL}
                        "${ZHLN_FENNELC}"
                        "${ZHLN_FENNEL_LUA}"
                        --use-bit-lib
                        --compile ${FNL_SRC} ${OUTPUT_LUA}
                DEPENDS ${FNL_SRC} "${ZHLN_FENNEL_LUA}" "${ZHLN_FENNELC}" ${ZHLN_FENNEL_DEPENDS}
                COMMENT "Fennel: Compiling ${FNL_SRC} -> ${OUTPUT_LUA}"
                VERBATIM
            )
        else()
            add_custom_command(
                OUTPUT ${OUTPUT_LUA}
                COMMAND ${FENNEL_BIN} --use-bit-lib --compile ${FNL_SRC} > ${OUTPUT_LUA}
                DEPENDS ${FNL_SRC}
                COMMENT "Fennel: Compiling ${FNL_SRC} -> ${OUTPUT_LUA}"
                VERBATIM
            )
        endif()
        list(APPEND LUA_OUTPUTS ${OUTPUT_LUA})
    endforeach()
    set(${OUTPUT_LUA_FILES_VAR} ${LUA_OUTPUTS} PARENT_SCOPE)
endfunction()
