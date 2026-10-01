# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


set(RAW_WINDOW_SOURCES
    PlatformHost.cpp
    PresentationTarget.cpp
    NativeSurfaceHandle.cpp
    Window.cpp
)

zahlen_transpile_sources(WINDOW_SOURCES ${RAW_WINDOW_SOURCES})

add_library(zahlen_window STATIC ${WINDOW_SOURCES})
set_target_properties(zahlen_window PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(WIN32)
    target_compile_definitions(zahlen_window PRIVATE ZHLN_WINDOW_NATIVE_WIN32=1)
elseif(NOT APPLE)
    if(_zhln_glfw_wayland)
        target_compile_definitions(zahlen_window PRIVATE ZHLN_WINDOW_NATIVE_WAYLAND=1)
    endif()
    if(_zhln_glfw_x11)
        target_compile_definitions(zahlen_window PRIVATE ZHLN_WINDOW_NATIVE_X11=1)
    endif()
endif()

target_compile_options(zahlen_window PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

target_link_libraries(zahlen_window
    PRIVATE
        glfw
)

target_include_directories(zahlen_window
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/engine
)

target_link_libraries(zahlen_window PRIVATE zahlen_warnings)
