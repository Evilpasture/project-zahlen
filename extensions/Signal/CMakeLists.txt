# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Signal -- caller-owned OS signal routing and crash diagnostics.
#
# Built unconditionally (like AssetCooking) so lightweight host tools (`ztest`),
# tests, samples, and engine frontends (`zahlen`, `zahlen_ui_editor`) can link
# `zahlen_signal` directly without pulling `zahlen_engine` into CLI-only tools.
# Public headers are included as <Signal/SignalManager.hpp>, <Signal/SignalSafe.hpp>,
# <Signal/SignalSafetyInspector.hpp>, and <Signal/CrashHandler.hpp>.
#
# Signal/WorkerCrashReports.cpp is what a *test worker* runs inside a fatal-signal
# handler, so it touches no allocator, no logger and no symbol resolver.
# Signal/StackSymbolizer.cpp is its parent-side counterpart -- it runs in `ztest`,
# after the worker is gone, and shells out to addr2line.

if(TARGET zahlen_signal)
    return()
endif()

set(TARGET_NAME zahlen_signal)

add_library(${TARGET_NAME} STATIC
    SignalManager.cpp
    StackSymbolizer.cpp
    WorkerCrashReports.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
    ${PROJECT_SOURCE_DIR}/include
)

# Match `zahlen_engine`'s `-fno-exceptions -fno-rtti` dialect on standalone
# consumers (`ztest`) and avoid mixed RTTI / `-fsanitize=vptr` COMDAT vtables.
target_compile_options(${TARGET_NAME} PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)

if(WIN32)
    # dbghelp: `SymbolResolver`-equivalent lookups for the win32 symbolizer path
    # (see the note in the root CMakeLists.txt). MSVC autolinks it from the
    # #pragma in the source; every other toolchain needs the import library.
    if(NOT MSVC)
        target_link_libraries(${TARGET_NAME} PRIVATE dbghelp)
    endif()
endif()

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

if(TARGET zahlen_warnings)
    target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
endif()
