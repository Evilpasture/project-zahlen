cmake_minimum_required(VERSION 3.25)
project(zahlen LANGUAGES C CXX OBJC OBJCXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional: use a release-like default for local builds
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Disable LLGL examples/tests in the superproject build.
set(LLGL_BUILD_TESTS OFF CACHE BOOL "Disable LLGL tests" FORCE)
set(LLGL_BUILD_EXAMPLES OFF CACHE BOOL "Disable LLGL examples" FORCE)
set(LLGL_BUILD_WRAPPER_C99 OFF CACHE BOOL "Disable LLGL C wrapper" FORCE)
set(LLGL_BUILD_WRAPPER_CSHARP OFF CACHE BOOL "Disable LLGL C# wrapper" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LLGL SYSTEM)

include(ExternalProject)
find_program(LUAJIT_MAKE NAMES gmake make REQUIRED)

set(LUAJIT_SOURCE_DIR ${CMAKE_SOURCE_DIR}/extern/LuaJIT)
set(LUAJIT_SRC_DIR ${LUAJIT_SOURCE_DIR}/src)

if(APPLE)
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(LUAJIT_MACOSX_DEPLOYMENT_TARGET 10.15)
  else()
    set(LUAJIT_MACOSX_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET})
  endif()
  set(LUAJIT_BUILD_COMMAND ${CMAKE_COMMAND} -E env "MACOSX_DEPLOYMENT_TARGET=${LUAJIT_MACOSX_DEPLOYMENT_TARGET}" ${LUAJIT_MAKE})
else()
  set(LUAJIT_BUILD_COMMAND ${LUAJIT_MAKE})
endif()

ExternalProject_Add(LuaJITProject
    SOURCE_DIR ${LUAJIT_SOURCE_DIR}
    BINARY_DIR ${LUAJIT_SRC_DIR}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${LUAJIT_BUILD_COMMAND}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${LUAJIT_SRC_DIR}/libluajit.a
)

add_library(LuaJIT STATIC IMPORTED GLOBAL)
set_target_properties(LuaJIT PROPERTIES
    IMPORTED_LOCATION ${LUAJIT_SRC_DIR}/libluajit.a
    INTERFACE_INCLUDE_DIRECTORIES ${LUAJIT_SRC_DIR}
)
add_dependencies(LuaJIT LuaJITProject)

# --- Jolt Physics Configuration ---
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_SSE OFF CACHE BOOL "" FORCE)
set(USE_NEON ON CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLES OFF CACHE BOOL "" FORCE)

# Add Jolt source
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)

foreach(dep_target LLGL LLGL_Metal LLGL_OpenGL Jolt)
    if(TARGET ${dep_target})
        if(MSVC)
            target_compile_options(${dep_target} PRIVATE /W0)
        else()
            # -w suppresses ALL warnings for these specific targets
            target_compile_options(${dep_target} PRIVATE -w)
        endif()
    endif()
endforeach()


# Collect source files
set(PROJECT_SOURCES
    src/main.cpp
    src/engine/AssertHandler.cpp
    src/engine/Render.cpp
    src/engine/Engine.cpp
    src/engine/Physics.cpp
    src/engine/AssetFactory.cpp
    src/engine/Window.cpp
    src/engine/Window.mm
    src/engine/Mutex.cpp
    src/engine/Thread.cpp
    src/engine/Thread.S
)

add_executable(zahlen ${PROJECT_SOURCES})
target_compile_definitions(zahlen PRIVATE 
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
    $<$<CONFIG:RelWithDebInfo>:JPH_RELEASE>
)
target_link_libraries(zahlen PRIVATE LLGL Jolt LuaJIT)
target_include_directories(zahlen SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
    ${CMAKE_SOURCE_DIR}/extern/LLGL/include
)

target_include_directories(zahlen PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/extern/LuaJIT/src
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zahlen PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()
