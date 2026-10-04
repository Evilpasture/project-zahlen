# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Console -- in-memory game console and the reflection debugger.
#
# A separate STATIC target, not part of the zahlen_extras aggregate: ConsoleDebugger
# talks to the Lua-independent scripting binder (ScriptBinder /
# ScriptECSBridge), and a host that only wants JSON or glTF must not be
# forced to compile that. Public headers are included as
# <Console/Console.hpp> / <Console/ConsoleDebugger.hpp>.

set(TARGET_NAME zahlen_console)

add_library(${TARGET_NAME} STATIC
    Console.cpp
    ConsoleDebugger.cpp
    SymbolResolver.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
    zahlen_scripting
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# SymbolResolver.cpp walks the symbol table through dbghelp on Windows, and its
# #pragma comment(lib, "dbghelp.lib") is MSVC-only -- MinGW needs the import
# library named instead. PRIVATE, the way ws2_32 is in extensions/net/Network and
# extensions/net/HTTP: the calls are compiled into this archive, so a consumer that
# includes SymbolResolver.hpp needs no dbghelp header of its own, and CMake still
# carries it through to that consumer's link line as a link-only dependency,
# which is where this archive's undefined __imp_SymSetOptions and friends get
# resolved.
if(MINGW)
    target_link_libraries(${TARGET_NAME} PRIVATE
        dbghelp
    )
endif()
