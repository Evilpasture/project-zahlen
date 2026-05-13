cmake_minimum_required(VERSION 3.25)

# 1. Force the compiler selection BEFORE project()
if(WIN32 AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(zahlen LANGUAGES C CXX ASM)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- 2. GLOBAL SANITIZER CONFIG ---
option(USE_SANITIZERS "Enable ASan and UBSan" OFF)
if(USE_SANITIZERS AND WIN32)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    add_definitions(-D__ASAN_ENABLED__)
endif()

# --- 3. GLFW Setup ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/glfw SYSTEM)


add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LuaJIT SYSTEM)

# --- 4. Jolt Physics ---
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
# Ensure Jolt doesn't try to use standard includes if your environment is being weird
set(USE_STD_INCLUDES ON CACHE BOOL "" FORCE) 
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)


add_subdirectory(${CMAKE_SOURCE_DIR}/extern/VulkanMemoryAllocator SYSTEM)

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/SPIRV-Reflect)

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/imgui)

add_subdirectory(${CMAKE_SOURCE_DIR}/src/render)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/threading)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/physics)  
add_subdirectory(${CMAKE_SOURCE_DIR}/src/ecs)

option(BUILD_TESTING "Build the render module test suite" OFF)
if(BUILD_TESTING)
    message("Building test suites for render backend")
    enable_testing()
endif()

# Collect source files
set(PROJECT_SOURCES
    src/main.cpp
    src/engine/Allocator.cpp
    src/engine/AssertHandler.cpp
    src/engine/Render.cpp
    src/engine/Engine.cpp
    src/engine/AssetFactory.cpp
    src/engine/Window.cpp
    src/engine/Input.cpp
    src/engine/Resources.c
    src/engine/Platform.cpp
    src/engine/Scripting.cpp
)

add_executable(zahlen)

set_target_properties(zahlen PROPERTIES ENABLE_EXPORTS ON)

target_sources(zahlen PRIVATE ${PROJECT_SOURCES})

target_sources(zahlen PRIVATE 
    ${CMAKE_SOURCE_DIR}/extern/stbi_impl.c 
    ${CMAKE_SOURCE_DIR}/extern/cgltf_impl.c
)

target_compile_definitions(zahlen PRIVATE 
    JPH_DOUBLE_PRECISION
)

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

# 5. Static Linking order
target_link_libraries(zahlen PRIVATE 
    zahlen_render
    zahlen_physics  
    zahlen_threading
    zahlen_ecs 
    glfw
    Jolt 
    LuaJIT
    spirv_reflect
    imgui
)

target_include_directories(zahlen SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
)
target_include_directories(zahlen PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src

    ${CMAKE_SOURCE_DIR}/extern/LuaJIT/src

    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
)

# DXC is bundled with Vulkan SDK and is the best tool for HLSL -> SPIR-V
find_program(DXC_EXECUTABLE NAMES dxc 
             PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")

if(DXC_EXECUTABLE)
    set(HLSL_SRC "${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl")
    
    # 1. Output to the BUILD directory (CMAKE_CURRENT_BINARY_DIR)
    set(VS_SPV   "${CMAKE_CURRENT_BINARY_DIR}/basic.hlsl.VSMain.spv")
    set(PS_SPV   "${CMAKE_CURRENT_BINARY_DIR}/basic.hlsl.PSMain.spv")

    add_custom_command(
        OUTPUT ${VS_SPV}
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -spirv ${HLSL_SRC} -Fo ${VS_SPV}
        DEPENDS ${HLSL_SRC}
        COMMENT "DXC: Compiling Vertex Shader to build directory"
    )

    add_custom_command(
        OUTPUT ${PS_SPV}
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -spirv ${HLSL_SRC} -Fo ${PS_SPV}
        DEPENDS ${HLSL_SRC}
        COMMENT "DXC: Compiling Pixel Shader to build directory"
    )
    
    add_custom_target(compile_shaders DEPENDS ${VS_SPV} ${PS_SPV})
    add_dependencies(zahlen compile_shaders)

    # 2. Tell the compiler to search the build directory for #embed files
    target_include_directories(zahlen PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()


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

# --- 6. RESOURCE LINKING (Zero-Copy) ---
# We use symlinks instead of copy_directory to avoid duplicating heavy assets like Sponza.
if(WIN32)
    add_custom_command(TARGET zahlen POST_BUILD
        # Remove physical directory if it exists (so we can replace it with a link)
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:zahlen>/resources"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:zahlen>/scripts"
        # Create the symlink
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:zahlen>/resources"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/scripts" "$<TARGET_FILE_DIR:zahlen>/scripts"
        COMMENT "Linking resources and scripts (Zero-Copy)..."
    )
else()
    # Unix/macOS logic
    add_custom_command(TARGET zahlen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:zahlen>/resources"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/scripts" "$<TARGET_FILE_DIR:zahlen>/scripts"
        COMMENT "Linking resources and scripts..."
    )
endif()

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