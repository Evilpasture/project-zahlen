# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# gameplay/RagdollAuthoring -- declarative ragdoll authoring
# (zahlen_ragdoll_authoring).

set(TARGET_NAME zahlen_ragdoll_authoring)

add_library(${TARGET_NAME} STATIC
    RagdollAuthoring.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

if(TARGET zahlen_lua)
    target_link_libraries(${TARGET_NAME} PRIVATE zahlen_lua)
    target_compile_definitions(${TARGET_NAME} PRIVATE ZHLN_RAGDOLL_HAS_LUA=1)
endif()

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/gameplay
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
