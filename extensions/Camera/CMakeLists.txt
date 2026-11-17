# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Target-camera policy, explicitly driven by a COM host.
# The Native* target below is a separate, transitional module adapter.

set(TARGET_NAME zahlen_camera)

set(ZHLN_CAMERA_SOURCES
    TargetCamera.cpp
)

add_library(${TARGET_NAME} STATIC ${ZHLN_CAMERA_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Only COM interfaces and POD engine components are used by the implementation.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

# IEntities component names/layouts use static reflection in the consumer.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# Transitional native callers only; never linked by the COM implementation.
add_library(zahlen_native_camera STATIC NativeTargetCamera.cpp)
set_target_properties(zahlen_native_camera PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_native_camera PUBLIC zahlen_camera PRIVATE zahlen_warnings)
zahlen_enable_reflection(zahlen_native_camera)
