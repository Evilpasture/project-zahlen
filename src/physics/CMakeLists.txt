# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


add_library(zahlen_physics STATIC
    Physics.cpp
    PhysicsWorld.hpp
    PhysicsWorld.cpp
    PhysicsContactEvents.hpp
    PhysicsSync.cpp
    PhysicsQuery.cpp
    PhysicsCommand.cpp
    ConstraintFactory.cpp
    PhysicsDebug.cpp
    PhysicsDebug.hpp
    RagdollFactory.cpp
)
set_target_properties(zahlen_physics PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(zahlen_physics
    PRIVATE
        Jolt
        zahlen_threading
)

# PhysicsWorld and the command/synchronization internals are private to this
# target; dependents receive the PhysicsContext façade only.
target_include_directories(zahlen_physics
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Pass Jolt specific configurations down
target_compile_definitions(zahlen_physics PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    JPH_DEBUG_RENDERER
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

target_compile_definitions(zahlen_physics PUBLIC
    JPH_DEBUG_RENDERER
)

target_compile_options(zahlen_physics PRIVATE -fno-exceptions -fno-rtti)

# This target once stripped -freflection from its own COMPILE_OPTIONS here,
# guarded to GCC; it was dead code twice over: the root's
# zahlen_enable_reflection(zahlen_physics) runs after this directory is
# configured (nothing had been added yet to strip), and Jolt's headers coexist
# with the reflection flags on every toolchain the build supports.
target_link_libraries(zahlen_physics PRIVATE zahlen_warnings)
