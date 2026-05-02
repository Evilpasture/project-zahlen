cmake_minimum_required(VERSION 3.25)

# 1. Force the compiler selection BEFORE project()
if(WIN32 AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(zahlen LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- 2. GLOBAL SANITIZER CONFIG ---
option(USE_SANITIZERS "Enable ASan and UBSan" ON)
if(USE_SANITIZERS AND WIN32)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    add_definitions(-D__ASAN_ENABLED__)
endif()

# --- 3. LLGL Static Setup (FIXED FLAGS) ---
set(LLGL_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LLGL_BUILD_RENDERER_VULKAN ON CACHE BOOL "" FORCE)
set(LLGL_BUILD_RENDERER_OPENGL ON CACHE BOOL "" FORCE)
set(LLGL_BUILD_RENDERER_DIRECT3D11 OFF CACHE BOOL "" FORCE)
set(LLGL_BUILD_RENDERER_DIRECT3D12 OFF CACHE BOOL "" FORCE) # FIXED: Removed extra "OFF"
set(LLGL_BUILD_RENDERER_METAL OFF CACHE BOOL "" FORCE)
set(LLGL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLGL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Force the internal LLGL toggles to OFF to stop the Static Registry from looking for them
set(LLGL_ENABLE_DIRECT3D11 OFF CACHE BOOL "" FORCE)
set(LLGL_ENABLE_DIRECT3D12 OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LLGL SYSTEM)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LuaJIT SYSTEM)

# --- 4. Jolt Physics ---
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
# Ensure Jolt doesn't try to use standard includes if your environment is being weird
set(USE_STD_INCLUDES ON CACHE BOOL "" FORCE) 
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)

# Collect source files
set(PROJECT_SOURCES
    src/main.cpp
    src/engine/Allocator.cpp
    src/engine/AssertHandler.cpp
    src/engine/Render.cpp
    src/engine/Engine.cpp
    src/engine/Physics.cpp
    src/engine/AssetFactory.cpp
    src/engine/Window.cpp
    src/engine/Mutex.cpp
    src/engine/Thread.cpp
    src/engine/Thread.S
    src/engine/Input.cpp
)

add_executable(zahlen ${PROJECT_SOURCES})

target_compile_definitions(zahlen PRIVATE 
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

# 5. Correct Static Linking order
# REMOVED: vulkan-1.lib (LLGL already brings it in with the full path)
target_link_libraries(zahlen PRIVATE 
    LLGL_Vulkan
    LLGL_OpenGL
    LLGL_Null
    LLGL
    Jolt 
    LuaJIT
)

target_include_directories(zahlen SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
    ${CMAKE_SOURCE_DIR}/extern/LLGL/include
)
target_include_directories(zahlen PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/extern/LuaJIT/src
)

# --- Compiler Flags ---
if(NOT MSVC)
  target_compile_options(zahlen PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
  if(WIN32)
    # This might help counteract the -nostdlib being injected by the environment
    target_link_options(zahlen PRIVATE -Xlinker /subsystem:console)
    target_compile_options(zahlen PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter)
    target_compile_options(zahlen PRIVATE -D_CRT_SECURE_NO_WARNINGS)
  endif()
endif()

# --- 6. RESOURCES & SANITIZER DLL ---
add_custom_command(TARGET zahlen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:zahlen>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/scripts" "$<TARGET_FILE_DIR:zahlen>/scripts"
    COMMENT "Copying resources and scripts..."
)

if(USE_SANITIZERS AND WIN32)
    get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    find_file(ASAN_DLL 
        NAMES "clang_rt.asan_dynamic-x86_64.dll"
        PATHS "${CLANG_BIN_DIR}/../lib/clang/23/lib/windows" "${CLANG_BIN_DIR}/../lib/clang/22/lib/windows"
    )
    if(ASAN_DLL)
        add_custom_command(TARGET zahlen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${ASAN_DLL}" "$<TARGET_FILE_DIR:zahlen>"
            COMMENT "Copying ASan Runtime..."
        )
    endif()
endif()