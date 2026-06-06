cmake_minimum_required(VERSION 3.25)

# 1. Force the compiler selection BEFORE project()
if(WIN32 AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(zahlen LANGUAGES C CXX ASM)

# --- FIX FOR LINUX/GCC 16 ASM COMPILATION RULES ---
if(UNIX OR APPLE)
    set(CMAKE_ASM_COMPILE_OBJECT "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>" CACHE STRING "ASM compile command" FORCE)
endif()

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fno-rtti -fno-exceptions")
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE MACOS_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_SYSROOT ${MACOS_SDK_PATH})
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

# --- DUAL-PATH DEV MODE OPTION ---
option(ZHLN_DEV_MODE "Enable raw loose-file asset loading for development" ON)
if(ZHLN_DEV_MODE)
    add_compile_definitions(ZHLN_DEV_MODE)
endif()

# --- 3. GLFW Setup ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/glfw SYSTEM)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LuaJIT SYSTEM)

# --- 4. Jolt Physics ---
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_STD_INCLUDES ON CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS ON CACHE BOOL "Build Jolt as shared library" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/VulkanMemoryAllocator SYSTEM)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/SPIRV-Reflect)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/imgui)
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(${CMAKE_SOURCE_DIR}/src/render)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/threading)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/physics)  
add_subdirectory(${CMAKE_SOURCE_DIR}/src/ecs)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/alife) 

option(BUILD_TESTING "Build the render module test suite" OFF)
if(BUILD_TESTING)
    message("Building test suites for render backend")
    enable_testing()
endif()

# Collect shared engine source files (EXCLUDING src/game_main.cpp)
set(ENGINE_SOURCES
    src/engine/Allocator.cpp
    src/engine/AssertHandler.cpp
    src/engine/RenderFrame.cpp
    src/engine/RenderInit.cpp
    src/engine/RenderResources.cpp
    src/engine/Engine.cpp
    src/engine/AssetFactory.cpp
    src/engine/MeshBuilder.cpp
    src/engine/glTFImporter.cpp
    src/engine/system/AnimationSystem.cpp
    src/engine/AssetManager.cpp
    src/engine/LevelLoader.cpp
    src/engine/Window.cpp
    src/engine/Input.cpp
    src/engine/Resources.c
    src/engine/Platform.cpp
    src/engine/Scripting.cpp
    src/engine/Physics_C.cpp
    src/engine/Buffer.cpp
    src/engine/ConsoleUI.cpp
    src/engine/ProfilerUI.cpp
    src/engine/Text.cpp
    src/engine/system/MovementSystem.cpp
    src/engine/Sky.cpp
    src/engine/BRDFLUT.cpp
)

find_library(ZSTD_LIBRARY NAMES zstd zstd_static HINTS "/opt/homebrew/lib" "/usr/local/lib" "/usr/lib")
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "Could not find zstd library. Please install it (e.g., 'brew install zstd').")
endif()

# --- 5. BUILD CORE SHARED LIBRARY ---
add_library(zahlen_engine SHARED ${ENGINE_SOURCES})

# Compile definitions required for DLL exports on Windows [3]
target_compile_definitions(zahlen_engine PRIVATE 
    ZHLN_ENGINE_BUILD
    JPH_DOUBLE_PRECISION
)

target_sources(zahlen_engine PRIVATE 
    ${CMAKE_SOURCE_DIR}/extern/stbi_impl.c 
    ${CMAKE_SOURCE_DIR}/extern/cgltf_impl.c
)

target_link_libraries(zahlen_engine PRIVATE 
    zahlen_render
    zahlen_physics  
    zahlen_threading
    zahlen_ecs 
    zahlen_alife 
    glfw
    LuaJIT
    spirv_reflect
    imgui
    ${ZSTD_LIBRARY}
)
# Jolt is part of the public ABI exposed by engine headers, so propagate its include paths.
target_link_libraries(zahlen_engine PUBLIC Jolt)

target_include_directories(zahlen_engine SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
)
# Use PUBLIC for the main include directory so dependent targets automatically get it [3]
target_include_directories(zahlen_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/LuaJIT/src
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
)

# --- 6. SHADER COMPILATION ---
# Handled within the shared library target
function(compile_shaders TARGET_NAME)
    set(SHADER_FILES ${ARGN})

    find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC not found!")
    endif()

    set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

    set(ALL_SPV_OUTPUTS "")
    set(STAGE_EXTS    "VS"      "PS"    )
    set(STAGE_ENTRIES "VSMain"  "PSMain")
    set(STAGE_PROFILES "vs_6_0" "ps_6_0")

    foreach(HLSL_SRC IN LISTS SHADER_FILES)
        get_filename_component(FILE_NAME_WE ${HLSL_SRC} NAME_WLE)
        get_filename_component(FILE_NAME ${HLSL_SRC} NAME)

        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${EXT}.spv")

            # Added: -I flag to resolve include paths, and explicitly tracked the common.hlsl dependency
            add_custom_command(
                OUTPUT ${OUTPUT_SPV}
                COMMAND ${DXC_EXECUTABLE} -T ${PROFILE} -E ${ENTRY} -spirv -I "${CMAKE_SOURCE_DIR}/resources/shaders" ${HLSL_SRC} -Fo ${OUTPUT_SPV}
                DEPENDS ${HLSL_SRC} "${CMAKE_SOURCE_DIR}/resources/shaders/common.hlsl"
                COMMENT "DXC: Generating ${FILE_NAME}.${EXT}.spv"
                VERBATIM
            )
            list(APPEND ALL_SPV_OUTPUTS ${OUTPUT_SPV})

            string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_${EXT}_PATH" MACRO_NAME)
            string(TOUPPER ${MACRO_NAME} MACRO_NAME)
            
            target_compile_definitions(${TARGET_NAME} PRIVATE ${MACRO_NAME}="${OUTPUT_SPV}")
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME}_shader_gen ALL DEPENDS ${ALL_SPV_OUTPUTS})
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_shader_gen)
    
    set_source_files_properties(src/engine/Resources.c PROPERTIES OBJECT_DEPENDS "${ALL_SPV_OUTPUTS}")
endfunction()

function(compile_hlsl SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC not found!")
    endif()

    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)
    set(OUTPUT_SPV "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/${FILE_NAME}.${ENTRY}.spv")

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${DXC_EXECUTABLE} -T ${STAGE} -E ${ENTRY} -spirv ${SHADER_PATH} -Fo ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH}
        COMMENT "DXC: Generating ${FILE_NAME}.${ENTRY}.spv"
        VERBATIM
    )
    set(${OUTPUT_VAR} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()

compile_shaders(zahlen_engine
    "${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/blit.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/taa.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/ui.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/toon.hlsl"
)

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/culling.hlsl" CSMain cs_6_0 CULLING_COMP_SPV)
add_custom_target(zahlen_engine_culling_shader ALL DEPENDS ${CULLING_COMP_SPV})
add_dependencies(zahlen_engine zahlen_engine_culling_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_CULLING_HLSL_CS_PATH="${CULLING_COMP_SPV}")
set_source_files_properties(src/engine/Resources.c PROPERTIES OBJECT_DEPENDS "${CULLING_COMP_SPV}")

# --- SPECIALIZED SHADOW FRAGMENT SHADER (Discards alpha masks without writing to rendering targets) ---
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl" PSShadow ps_6_0 SHADOW_FRAG_SPV)
add_custom_target(zahlen_engine_shadow_shader ALL DEPENDS ${SHADOW_FRAG_SPV})
add_dependencies(zahlen_engine zahlen_engine_shadow_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_SHADOW_HLSL_PS_PATH="${SHADOW_FRAG_SPV}")
set_source_files_properties(src/engine/Resources.c PROPERTIES OBJECT_DEPENDS "${SHADOW_FRAG_SPV}")

# --- 7. EXECUTABLE TARGETS ---

# Game Client Executable
add_executable(zahlen_game src/game_main.cpp)
target_link_libraries(zahlen_game PRIVATE zahlen_engine imgui)
target_compile_definitions(zahlen_game PRIVATE JPH_DOUBLE_PRECISION)

# Editor Executable
add_executable(zahlen_editor src/editor_main.cpp)
# We also link imgui directly so your editor source code can call raw ImGui functions
target_link_libraries(zahlen_editor PRIVATE zahlen_engine imgui)
target_compile_definitions(zahlen_editor PRIVATE JPH_DOUBLE_PRECISION)

# Offline Asset Cooker
add_executable(zahlen_cooker src/cooker_main.cpp)
target_link_libraries(zahlen_cooker PRIVATE zahlen_engine)
target_include_directories(zahlen_cooker PRIVATE ${CMAKE_SOURCE_DIR}/extern/cgltf)

# --- 7.1 ASSET COOKING AUTOMATION ---
add_custom_target(cook_assets ALL
    # 1. Make sure the data directory exists
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zahlen_game>/data"
    
    # 2. Run the cooker and output both base.pak AND debug .glb files [1]
    COMMAND zahlen_cooker "${CMAKE_SOURCE_DIR}/resources/assets" "$<TARGET_FILE_DIR:zahlen_game>/data/base.pak" "$<TARGET_FILE_DIR:zahlen_game>/data"
    
    DEPENDS zahlen_cooker
    COMMENT "Cooking assets into data/base.pak and compiling debug GLBs..."
)

# Ensure the game and editor wait for the cooking process to finish before they compile/run
add_dependencies(zahlen_game cook_assets)
add_dependencies(zahlen_editor cook_assets)

# --- 8. COMPILER FLAGS ---
if(NOT MSVC)
    target_compile_options(zahlen_engine PRIVATE $<$<CXX_COMPILER_ID:GNU>:--param=destructive-interference-size=64> -Wall -Wextra -Wpedantic -Wno-interference-size -fno-exceptions -fno-rtti)
    if(WIN32)
        target_link_options(zahlen_engine PRIVATE -Xlinker /subsystem:console)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter -Wno-microsoft-enum-forward-reference)
        target_compile_options(zahlen_engine PRIVATE -D_CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# --- 9. RESOURCE LINKING (Single target to avoid parallel race conditions) ---
add_custom_target(link_resources ALL
    COMMAND ${CMAKE_COMMAND} 
        -D SRC="${CMAKE_SOURCE_DIR}/resources" 
        -D DST="${CMAKE_CURRENT_BINARY_DIR}/resources" 
        -P "${CMAKE_SOURCE_DIR}/cmake/CreateSymlinks.cmake"
    COMMAND ${CMAKE_COMMAND} 
        -D SRC="${CMAKE_SOURCE_DIR}/scripts" 
        -D DST="${CMAKE_CURRENT_BINARY_DIR}/scripts" 
        -P "${CMAKE_SOURCE_DIR}/cmake/CreateSymlinks.cmake"
    COMMENT "Creating zero-copy symlinks for resources and scripts..."
)

# Make both executable targets depend on it so they wait for it to finish
add_dependencies(zahlen_game link_resources)
add_dependencies(zahlen_editor link_resources)



# --- 10. SANITIZER RUNTIME DISTRIBUTION ---
if(USE_SANITIZERS AND WIN32)
    get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    find_file(ASAN_DLL 
        NAMES "clang_rt.asan_dynamic-x86_64.dll"
        PATHS "${CLANG_BIN_DIR}/../lib/clang/23/lib/windows" "${CLANG_BIN_DIR}/../lib/clang/22/lib/windows"
    )
    if(ASAN_DLL)
        foreach(APP_TARGET zahlen_game zahlen_editor)
            add_custom_command(TARGET ${APP_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy "${ASAN_DLL}" "$<TARGET_FILE_DIR:${APP_TARGET}>"
                COMMENT "Copying ASan Runtime for ${APP_TARGET}..."
            )
        endforeach()
    endif()
endif()
