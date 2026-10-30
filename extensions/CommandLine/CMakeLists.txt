# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/CommandLine -- engine-independent argument parsing, help and
# version printing. EngineConfig binding and process commands live in app/.

if(TARGET zahlen_command_line)
    return()
endif()

add_library(zahlen_command_line STATIC
    CommandLine.cpp
)

set_target_properties(zahlen_command_line PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandLine.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
               COMPILE_DEFINITIONS
               "ZHLN_GIT_COMMIT_HASH=\"${ZHLN_GIT_HASH}\";ZHLN_COMPILER_FLAGS=\"${COMPILER_FLAGS_LIST}\";ZHLN_META_BUILDER=\"CMake ${CMAKE_VERSION}\";ZHLN_BUILD_TOOL=\"${CMAKE_GENERATOR}\";ZHLN_TARGET_TRIPLE=\"${ZHLN_TARGET_TRIPLE}\";ZHLN_LINKER_NAME=\"${ZHLN_LINKER}\""
)

target_include_directories(zahlen_command_line PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
    ${PROJECT_SOURCE_DIR}/include
)

# Standalone CLI targets (`ztest`) intentionally do not link `zahlen_engine`,
# so propagate the engine-wide C++ dialect (`-fno-exceptions -fno-rtti`)
# directly. Without `-fno-rtti`, GCC enables `-fsanitize=vptr` under
# `-fsanitize=undefined` on `CommandLine.cpp` while `-fno-rtti` consumers emit
# null `type_info*` slots (`vptr[-1] == nullptr`) in shared COMDAT vtables
# such as `libstdc++`'s `std::__format::_Sink<char>`.
target_compile_options(zahlen_command_line PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_command_line)
endif()

target_link_libraries(
    zahlen_command_line
    PUBLIC $<$<AND:$<BOOL:${MINGW}>,$<CXX_COMPILER_ID:GNU>>:stdc++exp>
)

if(TARGET zahlen_warnings)
    target_link_libraries(zahlen_command_line PRIVATE zahlen_warnings)
endif()
