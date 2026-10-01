# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


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

target_include_directories(zahlen_ecs
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(zahlen_ecs PRIVATE -fno-exceptions -fno-rtti)

# Signature-driven graph resolution includes SystemContext.hpp, whose public
# frame callbacks and visible lists use Jolt types. Give this target the same
# Jolt usage requirements (headers and ABI definitions) as other consumers.
target_link_libraries(zahlen_ecs PRIVATE Jolt zahlen_warnings)
