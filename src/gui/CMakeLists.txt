# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


add_library(zahlen_gui OBJECT
    GUIContext.cpp
    Text.cpp
    FontLoader.cpp
)
set_target_properties(zahlen_gui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

set(FONT_EMBEDDING_FILE "${CMAKE_CURRENT_SOURCE_DIR}/FontLoader.cpp")
set_source_files_properties(${FONT_EMBEDDING_FILE} PROPERTIES
    COMPILE_DEFINITIONS "ZHLN_DEFAULT_FONT_ZFONT_PATH=\"${PROJECT_SOURCE_DIR}/resources/fonts/DefaultFont.zfont\""
)
set_source_files_properties(${FONT_EMBEDDING_FILE} PROPERTIES
    OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/resources/fonts/DefaultFont.zfont"
)

target_include_directories(zahlen_gui
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(zahlen_gui SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/extern/clay
    ${PROJECT_SOURCE_DIR}/extern/stb
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
    ${PROJECT_SOURCE_DIR}/extern/volk
)

target_include_directories(zahlen_gui PRIVATE "${CMAKE_BINARY_DIR}/generated_shaders")
add_dependencies(zahlen_gui zahlen_gpu_types)

target_link_libraries(zahlen_gui PUBLIC
    zahlen_ecs
    zahlen_vulkan
    Jolt
)

target_link_libraries(zahlen_gui PRIVATE
    zahlen_threading
)

target_compile_definitions(zahlen_gui PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

target_compile_options(zahlen_gui PRIVATE
    -fno-exceptions
    -fno-rtti
    $<$<BOOL:${ZHLN_HAS_FBLOCKS}>:-fblocks>
)

target_link_libraries(zahlen_gui PRIVATE zahlen_warnings)
