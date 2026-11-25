# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

option(ZHLN_BUILD_SAMPLES "Build engine sample applications" ON)

# FirstGameSample.cpp is documentation for docs/FIRST_GAME.md. It is deliberately
# NOT a build target: an example that CI compiles is a fixture, and a changed
# example must never be able to break a release job. See docs/UPGRADING.md.
if(NOT ZHLN_BUILD_SAMPLES)
    return()
endif()

set(SAMPLE_SOURCES
    BSPMurderDronesSample.cpp
    CDNMapSample.cpp
    DeviceLostRecoverySample.cpp
    FidelityHarness.cpp
    GlTFInspectorSample.cpp
    ProceduralAnimationSample.cpp
    RemoteGLBSample.cpp
)

set(ZHLN_SAMPLE_OPTIONAL_TARGETS_ProceduralAnimationSample zahlen_animation zahlen_locomotion zahlen_procedural_animation zahlen_gltf_assets zahlen_terrain zahlen_character_controller zahlen_camera zahlen_ui)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_GlTFInspectorSample zahlen_gltf_assets zahlen_animation zahlen_camera zahlen_ui)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_RemoteGLBSample zahlen_gltf_assets zahlen_remote_asset zahlen_github zahlen_animation zahlen_ui)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_CDNMapSample zahlen_gltf_assets zahlen_cdn zahlen_freecam zahlen_animation)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_DeviceLostRecoverySample "")
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_FidelityHarness zahlen_gltf_assets zahlen_serialization zahlen_asset_cooking zahlen_netpbm zahlen_screenshot zahlen_animation)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_BSPMurderDronesSample zahlen_interaction zahlen_bsp_assets zahlen_scene zahlen_animation zahlen_locomotion zahlen_procedural_animation zahlen_gltf_assets zahlen_character_controller zahlen_camera zahlen_freecam zahlen_benchmark_viewer zahlen_ui)

set(ZHLN_MODULE_SAMPLES
    BSPMurderDronesSample
    ProceduralAnimationSample
)

foreach(SAMPLE_SRC IN LISTS SAMPLE_SOURCES)
    get_filename_component(SAMPLE_NAME ${SAMPLE_SRC} NAME_WE)

    set(ZHLN_SAMPLE_MISSING_OPTIONAL_TARGETS "")
    foreach(ZHLN_SAMPLE_OPTIONAL_TARGET IN LISTS ZHLN_SAMPLE_OPTIONAL_TARGETS_${SAMPLE_NAME})
        if(NOT TARGET ${ZHLN_SAMPLE_OPTIONAL_TARGET})
            list(APPEND ZHLN_SAMPLE_MISSING_OPTIONAL_TARGETS ${ZHLN_SAMPLE_OPTIONAL_TARGET})
        endif()
    endforeach()
    if(ZHLN_SAMPLE_MISSING_OPTIONAL_TARGETS)
        message(STATUS "Skipping sample ${SAMPLE_NAME}: optional target(s) not built: ${ZHLN_SAMPLE_MISSING_OPTIONAL_TARGETS}")
        continue()
    endif()

    add_executable(${SAMPLE_NAME} ${SAMPLE_SRC})

    set(ZHLN_SAMPLE_USES_MODULES OFF)
    if(SAMPLE_NAME IN_LIST ZHLN_MODULE_SAMPLES)
        set(ZHLN_SAMPLE_USES_MODULES ON)
    endif()

    set_target_properties(${SAMPLE_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES ${ZHLN_SAMPLE_USES_MODULES}
    )

    target_link_libraries(${SAMPLE_NAME} PRIVATE
        zahlen_application
        zahlen_signal
        zahlen_warnings
    )

    foreach(ZHLN_SAMPLE_OPTIONAL_TARGET IN LISTS ZHLN_SAMPLE_OPTIONAL_TARGETS_${SAMPLE_NAME})
        target_link_libraries(${SAMPLE_NAME} PRIVATE ${ZHLN_SAMPLE_OPTIONAL_TARGET})
    endforeach()

    if(TARGET zahlen_fonts)
        target_link_libraries(${SAMPLE_NAME} PRIVATE zahlen_fonts)
        target_compile_definitions(${SAMPLE_NAME} PRIVATE ZHLN_HAS_FONTS=1)
    endif()

    if(COMMAND zahlen_enable_reflection)
        zahlen_enable_reflection(${SAMPLE_NAME})
    endif()

    if(TARGET link_resources)
        add_dependencies(${SAMPLE_NAME} link_resources)
    endif()

    if(TARGET cook_assets)
        add_dependencies(${SAMPLE_NAME} cook_assets)
    endif()

    if(SAMPLE_NAME STREQUAL "DeviceLostRecoverySample" AND TARGET zahlen_plugin_freecam)
        # Build-order only: DeviceLostRecoverySample loads freecam dynamically
        # via PluginManifestRegistry + SharedLibrary::OpenPlugin without linking it.
        add_dependencies(${SAMPLE_NAME} zahlen_plugin_freecam)
    endif()

    if(USE_SANITIZERS AND WIN32 AND ASAN_DLL)
        add_custom_command(TARGET ${SAMPLE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASAN_DLL}" "$<TARGET_FILE_DIR:${SAMPLE_NAME}>"
            COMMENT "Copying ASan Runtime for ${SAMPLE_NAME}..."
        )
    endif()

    zahlen_add_runtime_dlls(${SAMPLE_NAME})
endforeach()
