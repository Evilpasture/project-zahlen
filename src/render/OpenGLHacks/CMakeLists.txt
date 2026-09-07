find_package(OpenGL REQUIRED)

add_library(HostBlitSwapchain STATIC HostBlitSwapchain.cpp)

set_target_properties(HostBlitSwapchain PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(HostBlitSwapchain PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/vulkan
)

if(NOT MSVC)
    target_compile_options(HostBlitSwapchain PRIVATE -fno-exceptions -fno-rtti)
endif()
target_link_libraries(HostBlitSwapchain PRIVATE zahlen_render)
target_link_libraries(HostBlitSwapchain PRIVATE zahlen_warnings)
target_link_libraries(HostBlitSwapchain PRIVATE OpenGL::GL)
target_link_libraries(HostBlitSwapchain PRIVATE glfw)

