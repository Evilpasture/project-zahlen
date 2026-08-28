# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


# src/audio/CMakeLists.txt

add_library(zahlen_audio STATIC
    AudioContext.cpp # <-- Keep only the context here
)
set_target_properties(zahlen_audio PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(zahlen_audio PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(zahlen_audio SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/extern/miniaudio
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
)

# Propagate dependency chains
target_link_libraries(zahlen_audio PUBLIC
    zahlen_ecs
    Jolt
)

target_link_libraries(zahlen_audio PRIVATE
    zahlen_threading
)

# Apply global Jolt macro definitions to match Engine alignment (prevents silent ABI crashes)
target_compile_definitions(zahlen_audio PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

if(NOT MSVC)
    target_compile_options(zahlen_audio PRIVATE
        -fno-exceptions
        -fno-rtti
        $<$<CXX_COMPILER_ID:Clang>:-fblocks>
        $<$<CXX_COMPILER_ID:AppleClang>:-fblocks>
    )
endif()

target_link_libraries(zahlen_audio PRIVATE zahlen_warnings)
