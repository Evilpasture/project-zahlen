cmake_minimum_required(VERSION 3.25)
project(imgui LANGUAGES CXX)

# 1. Define the source files
set(IMGUI_SOURCES
    imgui.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    imgui_demo.cpp
    backends/imgui_impl_glfw.cpp
    backends/imgui_impl_vulkan.cpp
)

# 2. Create the static library
add_library(imgui STATIC ${IMGUI_SOURCES})

# ISOLATE LANGUAGE STANDARD FOR THIRD-PARTY CODE
# This forces ImGui and its Apple dependencies to use C++23.
# Your main project target can still freely use C++26.
set_target_properties(imgui PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# 3. Public includes so the engine can find <imgui.h>
target_include_directories(imgui PUBLIC .)
target_include_directories(imgui PUBLIC backends)

# 4. Link dependencies 
find_package(Vulkan REQUIRED)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

# 5. Disable compiler warnings for third-party code
if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
else()
    target_compile_options(imgui PRIVATE /W0)
endif()