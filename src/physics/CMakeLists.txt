# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


add_library(zahlen_physics STATIC
    Physics.cpp
    BodyTable.hpp
    CharacterManager.hpp
    CharacterManager.cpp
    CommandMailbox.hpp
    PhysicsWorld.hpp
    PhysicsWorld.cpp
    PhysicsContactEvents.hpp
    PhysicsSync.cpp
    PhysicsQuery.cpp
    PhysicsCommand.cpp
    ConstraintFactory.cpp
    PhysicsDebug.cpp
    PhysicsDebug.hpp
    RagdollManager.hpp
    RagdollFactory.cpp
    ShapeCache.hpp
    ShapeCache.cpp
    TransformBuffer.hpp
)
set_target_properties(zahlen_physics PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# zahlen_threading is an OBJECT library linked directly into zahlen_engine;
# naming it here would embed duplicate copies of its objects in libzahlen_physics.a.
target_link_libraries(zahlen_physics
    PRIVATE
        Jolt
)

target_include_directories(zahlen_physics
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(zahlen_physics PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    JPH_DEBUG_RENDERER
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)


target_compile_options(zahlen_physics PRIVATE -fno-exceptions -fno-rtti)

if(ZHLN_ENABLE_PCH AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_precompile_headers(zahlen_physics PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}/include/Zahlen/Math/Math3D.hpp>"
        "$<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}/include/Zahlen/Log.hpp>"
    )
endif()

target_link_libraries(zahlen_physics PRIVATE zahlen_warnings)
