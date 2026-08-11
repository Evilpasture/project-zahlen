# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# src/ecs/CMakeLists.txt

add_library(zahlen_ecs OBJECT
    ECS.cpp
    SystemGraph.cpp
    EntityCommandBuffer.cpp
)
set_target_properties(zahlen_ecs PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(zahlen_ecs PUBLIC 
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

if(NOT MSVC)
    target_compile_options(zahlen_ecs PRIVATE -fno-exceptions -fno-rtti)
endif()
