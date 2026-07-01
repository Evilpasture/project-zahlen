# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# ----------------------------------------------------------------------------
# zahlen_demo_utils — platform-specific demo window helpers
# Compile this module with C++23 to avoid Apple Objective-C++ C++26 compatibility issues.
# ----------------------------------------------------------------------------

set(DEMO_UTILS_SRCS
    DemoWindow.hpp
)

if(APPLE)
    list(APPEND DEMO_UTILS_SRCS DemoWindow_mac.mm)
elseif(WIN32)
    list(APPEND DEMO_UTILS_SRCS DemoWindow_win32.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND DEMO_UTILS_SRCS DemoWindow_linux.cpp)
endif()

add_library(zahlen_demo_utils STATIC ${DEMO_UTILS_SRCS})

target_link_libraries(zahlen_demo_utils PUBLIC
    zahlen_render
    Vulkan::Vulkan
)

set_target_properties(zahlen_demo_utils PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_compile_features(zahlen_demo_utils PRIVATE cxx_std_23)

if(APPLE)
    set_source_files_properties(DemoWindow_mac.mm PROPERTIES LANGUAGE OBJCXX)
    target_link_libraries(zahlen_demo_utils PUBLIC "-framework Cocoa" "-framework QuartzCore")
elseif(WIN32)
    target_link_libraries(zahlen_demo_utils PUBLIC user32.lib kernel32.lib)
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(zahlen_demo_utils PUBLIC X11)
    target_compile_definitions(zahlen_demo_utils PUBLIC VK_USE_PLATFORM_XLIB_KHR)
endif()
