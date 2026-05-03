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

# At the bottom of src/render/CMakeLists.txt
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------------------
# Triangle App
# ----------------------------------------------------------------------------

# Find the glslangValidator compiler included in the Vulkan SDK
find_program(GLSL_COMPILER glslangValidator HINTS $ENV{VULKAN_SDK}/Bin)

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/triangle_app")
set(VERT_SRC "${SHADER_DIR}/triangle.vert")
set(FRAG_SRC "${SHADER_DIR}/triangle.frag")

# Define custom commands to compile the shaders to .spv
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/triangle.vert.spv"
    COMMAND ${GLSL_COMPILER} -V ${VERT_SRC} -o "${CMAKE_CURRENT_BINARY_DIR}/triangle.vert.spv"
    DEPENDS ${VERT_SRC}
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/triangle.frag.spv"
    COMMAND ${GLSL_COMPILER} -V ${FRAG_SRC} -o "${CMAKE_CURRENT_BINARY_DIR}/triangle.frag.spv"
    DEPENDS ${FRAG_SRC}
)

add_executable(zahlen_triangle
    triangle_app/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/triangle.vert.spv"
    "${CMAKE_CURRENT_BINARY_DIR}/triangle.frag.spv"
)

target_link_libraries(zahlen_triangle PRIVATE zahlen_render Vulkan::Vulkan)
target_include_directories(zahlen_triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zahlen_triangle PRIVATE cxx_std_23)

if(MSVC) # Required for Win32 subsystem API links
    target_link_libraries(zahlen_triangle PRIVATE user32.lib kernel32.lib)
endif()