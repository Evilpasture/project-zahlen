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
)

# 2. Create the static library
add_library(imgui STATIC ${IMGUI_SOURCES})

set_target_properties(imgui PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# 3. Mark include directories as SYSTEM PUBLIC so consumers treat imgui.h as system headers
target_include_directories(imgui SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends"
)

# 4. Link dependencies
find_package(Vulkan REQUIRED)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

# 5. Disable compiler warnings when compiling ImGui .cpp files
if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
else()
    target_compile_options(imgui PRIVATE /W0)
endif()
