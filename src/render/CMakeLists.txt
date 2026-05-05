# ----------------------------------------------------------------------------
# zahlen_render — Vulkan/VMA leaf module
# No engine dependencies. Vulkan + VMA only.
# ----------------------------------------------------------------------------

find_package(Vulkan REQUIRED)

add_library(zahlen_render STATIC
    RenderCore.c
    VmaImplementation.cpp
    RenderCore.h
    RenderCore.hpp
    Allocator.hpp
    Utils.h
)

# --- OPTIONAL CLANG-TIDY SUPPORT ---
option(ZHLN_ENABLE_CLANG_TIDY "Run Clang-Tidy analysis during compilation" OFF)

if(ZHLN_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set_target_properties(zahlen_render PROPERTIES
            C_CLANG_TIDY "${CLANG_TIDY_EXE}"
            CXX_CLANG_TIDY "${CLANG_TIDY_EXE}"
        )
    else()
        message(WARNING "zahlen_render: Clang-Tidy requested but executable not found.")
    endif()
endif()

set_source_files_properties(RenderCore.c PROPERTIES
    LANGUAGE C
    C_STANDARD 23
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_compile_features(zahlen_render PUBLIC cxx_std_23)
set_target_properties(zahlen_render PROPERTIES CXX_EXTENSIONS OFF)

target_link_libraries(zahlen_render PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    spirv_reflect
)

target_include_directories(zahlen_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(zahlen_render PRIVATE
    VMA_IMPLEMENTATION
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zahlen_render PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------------------
# Demo Utilities (Shared PIMPL Window Abstraction)
# ----------------------------------------------------------------------------

set(DEMO_UTILS_SRCS demo_utils/DemoWindow.hpp)

if(APPLE)
    list(APPEND DEMO_UTILS_SRCS demo_utils/DemoWindow_mac.mm)
elseif(WIN32)
    list(APPEND DEMO_UTILS_SRCS demo_utils/DemoWindow_win32.cpp)
endif()

add_library(zahlen_demo_utils STATIC ${DEMO_UTILS_SRCS})
target_link_libraries(zahlen_demo_utils PRIVATE zahlen_render Vulkan::Vulkan)
target_include_directories(zahlen_demo_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(APPLE)
    # Compile .mm files as Objective-C++
    set_source_files_properties(demo_utils/DemoWindow_mac.mm PROPERTIES LANGUAGE OBJCXX)
    target_link_libraries(zahlen_demo_utils PUBLIC "-framework Cocoa" "-framework QuartzCore")
elseif(WIN32)
    target_link_libraries(zahlen_demo_utils PUBLIC user32.lib kernel32.lib)
endif()

# ----------------------------------------------------------------------------
# HLSL Shader Compilation (using DXC from Vulkan SDK)
# ----------------------------------------------------------------------------

# 1. Locate DXC
find_program(DXC_COMPILER dxc HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin)

if(NOT DXC_COMPILER)
    message(FATAL_ERROR "zahlen_render: DXC compiler not found! Ensure VULKAN_SDK is set in your environment.")
endif()

# 2. Define the HLSL compilation function
# Usage: compile_hlsl(FILE ENTRY_POINT STAGE[vs/ps] OUTPUT_VAR)
function(compile_hlsl SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)
    
    # Generate a unique output name (e.g., cube.hlsl.VSMain.spv)
    set(SPV_OUT "${CMAKE_CURRENT_BINARY_DIR}/${FILE_NAME}.${ENTRY}.spv")
    
    add_custom_command(
        OUTPUT "${SPV_OUT}"
        COMMAND ${DXC_COMPILER} 
                -T ${STAGE}_6_0   # Target Profile: vs_6_0 or ps_6_0
                -E ${ENTRY}       # Entry Point name
                -spirv            # Output SPIR-V
                -fspv-target-env=vulkan1.3 
                "${SHADER_PATH}" 
                -Fo "${SPV_OUT}"
        DEPENDS "${SHADER_PATH}"
        COMMENT "Compiling HLSL: ${FILE_NAME} [${ENTRY}] -> ${STAGE}"
    )
    set(${OUTPUT_VAR} "${SPV_OUT}" PARENT_SCOPE)
endfunction()

# 3. Compile the Shaders
# Note: You can now put Vertex and Fragment shaders in the same .hlsl file!
set(CUBE_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/cube_app/cube.hlsl")
compile_hlsl("${CUBE_HLSL}" VSMain vs CUBE_VERT_SPV)
compile_hlsl("${CUBE_HLSL}" PSMain ps CUBE_FRAG_SPV)

set(TRI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/triangle_app/triangle.hlsl")
compile_hlsl("${TRI_HLSL}" VSMain vs TRI_VERT_SPV)
compile_hlsl("${TRI_HLSL}" PSMain ps TRI_FRAG_SPV)

set(CRT_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/crt_app/crt.hlsl")
compile_hlsl("${CRT_HLSL}" VSMain vs CRT_VERT_SPV)
compile_hlsl("${CRT_HLSL}" PSMain ps CRT_FRAG_SPV)

set(SPONZA_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/sponza_app/sponza.hlsl")
set(SPONZA_SHADOW_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/sponza_app/shadow.hlsl")
compile_hlsl("${SPONZA_HLSL}" VSMain vs SPONZA_VERT_SPV)
compile_hlsl("${SPONZA_HLSL}" PSMain ps SPONZA_FRAG_SPV)
compile_hlsl("${SPONZA_SHADOW_HLSL}" VSMain vs SPONZA_SHADOW_VERT_SPV)
compile_hlsl("${SPONZA_SHADOW_HLSL}" PSMain ps SPONZA_SHADOW_FRAG_SPV)

# 4. Triangle Executable
add_executable(zahlen_triangle triangle_app/main.cpp ${TRI_VERT_SPV} ${TRI_FRAG_SPV})
target_link_libraries(zahlen_triangle PRIVATE zahlen_render Vulkan::Vulkan zahlen_demo_utils)
target_include_directories(zahlen_triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zahlen_triangle PRIVATE cxx_std_23)

# 5. Cube Executable
add_executable(zahlen_cube cube_app/main.cpp ${CUBE_VERT_SPV} ${CUBE_FRAG_SPV})
target_link_libraries(zahlen_cube PRIVATE zahlen_render Vulkan::Vulkan zahlen_demo_utils)
target_include_directories(zahlen_cube PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zahlen_cube PRIVATE cxx_std_23)

# 6. CRT Executable
add_executable(zahlen_crt crt_app/main.cpp ${CRT_VERT_SPV} ${CRT_FRAG_SPV})
target_link_libraries(zahlen_crt PRIVATE zahlen_render Vulkan::Vulkan zahlen_demo_utils)
target_include_directories(zahlen_crt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zahlen_crt PRIVATE cxx_std_23)

# 7. Sponza Executable
add_executable(zahlen_sponza sponza_app/main.cpp 
    ${SPONZA_VERT_SPV} 
    ${SPONZA_FRAG_SPV} 
    ${SPONZA_SHADOW_VERT_SPV} 
    ${SPONZA_SHADOW_FRAG_SPV}
)
target_link_libraries(zahlen_sponza PRIVATE zahlen_render Vulkan::Vulkan zahlen_demo_utils)
target_include_directories(zahlen_sponza PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
)
target_compile_features(zahlen_crt PRIVATE cxx_std_23)