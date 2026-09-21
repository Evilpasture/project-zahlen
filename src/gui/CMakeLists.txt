# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# src/gui/CMakeLists.txt

add_library(zahlen_gui OBJECT
    GUIContext.cpp
    Text.cpp
)
set_target_properties(zahlen_gui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# GUI implementation headers must not leak to dependent subsystems.
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

# The generated GPU host types (see cmake/ShaderCompilation.cmake): this
# target includes <Zahlen/Components.hpp> and <Zahlen/Render.hpp>, which reach
# <GeneratedGpuTypes.hpp> through <Zahlen/GpuLayout.hpp>, so the reflection
# runs before this target does. The path is spelled directly because this
# directory is processed before ShaderCompilation.cmake defines its variable.
target_include_directories(zahlen_gui PRIVATE "${CMAKE_BINARY_DIR}/generated_shaders")
add_dependencies(zahlen_gui zahlen_gpu_types)

# Propagate dependency chains
target_link_libraries(zahlen_gui PUBLIC
    zahlen_ecs
    zahlen_vulkan
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
