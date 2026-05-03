# src/render/tests/CMakeLists.txt

find_package(Vulkan REQUIRED)

# ----------------------------------------------------------------------------
# Test executable
# ----------------------------------------------------------------------------

add_executable(zahlen_render_tests
    main.cpp
    test_utils.cpp           # ZHLN_Clamp, ZHLN_VkResultString
    test_device.cpp          # instance, device selection, creation
    test_swapchain_support.cpp # ZHLN_QuerySwapchainSupport (headless)
    test_buffer.cpp          # Buffer::Create, Map, UploadToBuffer
    test_sync.cpp            # ZHLN_CreateFrameSync, destroy
    test_pipeline.cpp        # shader module creation, pipeline layout
    test_upload.cpp    # New
    test_raii.cpp      # New
    test_image.cpp     # New
)

target_link_libraries(zahlen_render_tests PRIVATE
    zahlen_render
    Vulkan::Vulkan
)

target_include_directories(zahlen_render_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..   # RenderCore.h, RenderCore.hpp
)

target_compile_features(zahlen_render_tests PRIVATE cxx_std_23)
set_target_properties(zahlen_render_tests PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zahlen_render_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ----------------------------------------------------------------------------
# CTest integration
# ----------------------------------------------------------------------------

enable_testing()

# Headless — no GPU required, always run
add_test(NAME render.utils    COMMAND zahlen_render_tests utils)
add_test(NAME render.errors   COMMAND zahlen_render_tests errors)

# Device tests — skip gracefully if no Vulkan device is present
add_test(NAME render.device   COMMAND zahlen_render_tests device)
add_test(NAME render.buffer   COMMAND zahlen_render_tests buffer)
add_test(NAME render.sync     COMMAND zahlen_render_tests sync)
add_test(NAME render.pipeline COMMAND zahlen_render_tests pipeline)
add_test(NAME render.swapchain COMMAND zahlen_render_tests swapchain)
add_test(NAME render.upload    COMMAND zahlen_render_tests upload)
add_test(NAME render.raii      COMMAND zahlen_render_tests raii)
add_test(NAME render.image     COMMAND zahlen_render_tests image)


# Mark device tests as optional so CI doesn't fail on headless runners
set_tests_properties(
    render.device
    render.buffer
    render.sync
    render.pipeline
    render.swapchain
    render.upload      # New
    render.raii        # New
    render.image       # New
    PROPERTIES SKIP_RETURN_CODE 77
)