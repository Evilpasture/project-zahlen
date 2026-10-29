# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Scripting -- Lua-independent reflection property inspection & ECS bridge.
#
# Provides the reflection-driven property table (`ScriptBinder`), component
# registry (`ScriptBinderRegistry`), scalar/Jolt value traits (`ScriptValueTypes`),
# and ECS state inspection/mutation (`ScriptECSBridge`) consumed by `ConsoleDebugger`.
# Nothing here includes a Lua header or exposes imperative method dispatch.

set(TARGET_NAME zahlen_scripting)

add_library(${TARGET_NAME} STATIC
    ScriptECSBridge.cpp
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

# Public headers are included as <Scripting/...> by consumers, so the extensions
# root is the only include path this target needs to publish.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

zahlen_enable_reflection(${TARGET_NAME})
