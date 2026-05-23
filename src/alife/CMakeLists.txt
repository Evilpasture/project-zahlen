# src/alife/CMakeLists.txt

set(ALIFE_SOURCES
    Factions.cpp
    GOAP.cpp
    Graph.cpp
    Simulator.cpp
    SpatialGrid.cpp
)

# Create a static library for the ALife subsystem
add_library(zahlen_alife STATIC ${ALIFE_SOURCES})
set_target_properties(zahlen_alife PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Disable MSVC warnings for secure CRT functions if compiling on Windows
target_compile_definitions(zahlen_alife PRIVATE _CRT_SECURE_NO_WARNINGS)

# Include directories
target_include_directories(zahlen_alife PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
)

target_include_directories(zahlen_alife PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# ALife inherently relies on the ECS and Jolt Physics (for RVec3, math, etc.)
target_link_libraries(zahlen_alife PUBLIC
    zahlen_ecs
    Jolt
)

# Optional: Disable RTTI and Exceptions to match the rest of the engine (if not on MSVC)
if(NOT MSVC)
    target_compile_options(zahlen_alife PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()