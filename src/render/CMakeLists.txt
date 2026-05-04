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
# Triangle and Cube Apps
# ----------------------------------------------------------------------------

find_program(GLSL_COMPILER glslangValidator HINTS $ENV{VULKAN_SDK}/Bin)

function(compile_shader SHADER_PATH OUTPUT_VAR)
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)
    set(SPV_OUT "${CMAKE_CURRENT_BINARY_DIR}/${FILE_NAME}.spv")
    add_custom_command(
        OUTPUT "${SPV_OUT}"
        COMMAND ${GLSL_COMPILER} -V "${SHADER_PATH}" -o "${SPV_OUT}"
        DEPENDS "${SHADER_PATH}"
        COMMENT "Compiling ${FILE_NAME}"
    )
    set(${OUTPUT_VAR} "${SPV_OUT}" PARENT_SCOPE)
endfunction()

compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/triangle_app/triangle.vert" TRI_VERT_SPV)
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/triangle_app/triangle.frag" TRI_FRAG_SPV)
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/cube_app/cube.vert" CUBE_VERT_SPV)
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/cube_app/cube.frag" CUBE_FRAG_SPV)

# Triangle Executable
add_executable(zahlen_triangle triangle_app/main.cpp ${TRI_VERT_SPV} ${TRI_FRAG_SPV})
target_link_libraries(zahlen_triangle PRIVATE 
    zahlen_render 
    Vulkan::Vulkan 
    zahlen_demo_utils
)
target_include_directories(zahlen_triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zahlen_triangle PRIVATE cxx_std_23)

# Cube Executable
add_executable(zahlen_cube cube_app/main.cpp ${CUBE_VERT_SPV} ${CUBE_FRAG_SPV})
target_link_libraries(zahlen_cube PRIVATE 
    zahlen_render 
    Vulkan::Vulkan 
    zahlen_demo_utils
)
target_include_directories(zahlen_cube PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zahlen_cube PRIVATE cxx_std_23)