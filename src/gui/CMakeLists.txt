# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# src/gui/CMakeLists.txt

add_library(zahlen_gui OBJECT
    GUIContext.cpp
    GUIEditor.cpp
    Text.cpp
)
set_target_properties(zahlen_gui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(zahlen_gui PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_include_directories(zahlen_gui SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/extern/clay
    ${PROJECT_SOURCE_DIR}/extern/stb
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
    ${PROJECT_SOURCE_DIR}/extern/volk
)

# Propagate dependency chains
target_link_libraries(zahlen_gui PUBLIC
    zahlen_ecs
    zahlen_render
    Jolt
)

target_link_libraries(zahlen_gui PRIVATE
    zahlen_threading
)

# Apply global Jolt macro definitions to match Engine alignment (prevents silent ABI crashes)
target_compile_definitions(zahlen_gui PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

if(NOT MSVC)
    target_compile_options(zahlen_gui PRIVATE
        -fno-exceptions
        -fno-rtti
        $<$<CXX_COMPILER_ID:Clang>:-fblocks>
        $<$<CXX_COMPILER_ID:AppleClang>:-fblocks>
    )
endif()

target_link_libraries(zahlen_gui PRIVATE zahlen_warnings)
