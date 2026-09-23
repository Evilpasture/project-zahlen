# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Camera -- third-person target camera rig (zahlen_camera).
#
# The spring-arm orbit camera (zoom, pitch clamps, smoothing) and its free-cam
# branch are gameplay policy, not render substrate: core keeps the minimal
# Camera, CameraComponent and CameraSystem. This target owns the rig's
# component and system and contributes its frame step through the engine's
# FrameSchedulerExtension seam, before the core "CameraSystems" step.
#
# Public headers are reached as <Camera/...>.

set(TARGET_NAME zahlen_camera)

file(GLOB ZHLN_CAMERA_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
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

# Core is a public dependency: the rig runs on the ECS, Camera and the frame
# scheduler extension seam.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# RegisterComponent names the component through ZHLN::Reflect, which needs
# the static reflection flags every component-registering domain carries.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
