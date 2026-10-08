# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later



# Keep miniaudio's implementation in a C translation unit: its Apple
# CoreAudio/AudioToolbox implementation includes SDK declarations using Blocks.
# If the configured project C compiler cannot parse those headers, only this C
# target is routed through the Blocks-capable compiler selected in ThirdParty.cmake.
add_library(zahlen_miniaudio STATIC
    MiniaudioImpl.c
)
set_target_properties(zahlen_miniaudio PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
target_include_directories(zahlen_miniaudio SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/miniaudio
)
if(APPLE)
    target_compile_options(zahlen_miniaudio PRIVATE -fblocks)
    if(NOT ZHLN_C_BLOCKS_SUPPORTED)
        set_property(TARGET zahlen_miniaudio PROPERTY C_COMPILER_LAUNCHER
            "${ZHLN_BLOCKS_C_COMPILER_LAUNCHER}"
        )
    endif()
endif()

add_library(zahlen_audio STATIC
    AudioContext.cpp
)
set_target_properties(zahlen_audio PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(zahlen_audio
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(zahlen_audio SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/extern/miniaudio
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
)

target_link_libraries(zahlen_audio PUBLIC
    zahlen_ecs
    Jolt
)

target_link_libraries(zahlen_audio PRIVATE
    zahlen_threading
    zahlen_miniaudio
)

target_compile_definitions(zahlen_audio PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

target_compile_options(zahlen_audio PRIVATE
    -fno-exceptions
    -fno-rtti
    $<$<BOOL:${ZHLN_HAS_FBLOCKS}>:-fblocks>
)

target_link_libraries(zahlen_audio PRIVATE zahlen_warnings)
