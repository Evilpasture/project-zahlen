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

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- 2. GLOBAL SANITIZER CONFIG ---
option(USE_SANITIZERS "Enable ASan and UBSan" ON)
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
add_subdirectory(${CMAKE_SOURCE_DIR}/src/render)

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
    src/engine/Physics.cpp
    src/engine/AssetFactory.cpp
    src/engine/Window.cpp
    src/engine/Mutex.cpp
    src/engine/Thread.cpp
    src/engine/Thread.S
    src/engine/Input.cpp
    src/engine/Resources.c
    src/engine/Platform.cpp
)

add_executable(zahlen ${PROJECT_SOURCES})

target_compile_definitions(zahlen PRIVATE 
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

# 5. Static Linking order
target_link_libraries(zahlen PRIVATE 
    zahlen_render
    glfw
    Jolt 
    LuaJIT
)

target_include_directories(zahlen SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
)
target_include_directories(zahlen PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/extern/LuaJIT/src
)

# --- Find glslangValidator in Vulkan SDK ---
find_program(GLSL_VALIDATOR NAMES glslangValidator 
             PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")

if(GLSL_VALIDATOR)
    message(STATUS "Found glslangValidator: ${GLSL_VALIDATOR}")
    
    set(SHADER_SOURCES 
        "${CMAKE_SOURCE_DIR}/resources/shaders/basic.vert"
        "${CMAKE_SOURCE_DIR}/resources/shaders/basic.frag"
    )
    
    foreach(SHADER ${SHADER_SOURCES})
        get_filename_component(FILE_NAME ${SHADER} NAME)
        set(OUTPUT_SPV "${CMAKE_SOURCE_DIR}/resources/shaders/${FILE_NAME}.spv")
        
        # This command runs glslangValidator during the build
        add_custom_command(
            OUTPUT ${OUTPUT_SPV}
            COMMAND ${GLSL_VALIDATOR} -V ${SHADER} -o ${OUTPUT_SPV}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${FILE_NAME} to SPIR-V"
        )
        list(APPEND SPV_OUTPUTS ${OUTPUT_SPV})
    endforeach()
    
    # Add the SPV files as a dependency to the main executable
    add_custom_target(compile_shaders DEPENDS ${SPV_OUTPUTS})
    add_dependencies(zahlen compile_shaders)
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