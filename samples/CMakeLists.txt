# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

option(ZHLN_BUILD_SAMPLES "Build engine sample applications" ON)

if(NOT ZHLN_BUILD_SAMPLES)
    return()
endif()

file(GLOB SAMPLE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# Which optional-layer targets each sample consumes (imports, includes, or installs),
# by sample name. A sample that appears in none of these lists links no optional targets.
#
#   ProceduralAnimationSample  imports ZHLN.Locomotion and
#                              ZHLN.ProceduralAnimation (zahlen_animation),
#                              includes <glTF/GLTFImporter.hpp> (zahlen_gltf)
#                              to load the rig it animates, builds its
#                              ground with <Terrain/TerrainFactory.hpp>
#                              (zahlen_terrain), and drives its third-person
#                              view through the extension camera rig
#                              (<Camera/TargetCamera.hpp>, zahlen_camera), and
#                              installs the character controller that drives
#                              its WASD and physics steps
#                              (<CharacterController/CharacterController.hpp>,
#                              zahlen_character_controller).
#   GlTFInspectorSample        imports ZHLN.glTF (zahlen_gltf).
#   RemoteGLBSample            fetches through ZHLN::Remote (zahlen_remote_asset
#                              over extensions/net/HTTP) and imports with ZHLN.glTF
#                              (zahlen_gltf); its model-list crawl goes through
#                              ZHLN::GitHub (zahlen_github, over extensions/net/HTTP
#                              and plugins/json).
#   CDNMapSample               ZHLN::CDN::CDNManager Load of one R2 .glb, then
#                              zahlen_gltf import; core free-cam.
#   FidelityHarness            headless glTF-Render-Fidelity runner: imports
#                              with ZHLN.glTF (zahlen_gltf), reads the Khronos
#                              scenario JSONs with ZHLN::ReflectJSON
#                              (zahlen_serialization -- plugins/json), and
#                              supplies images through zahlen_asset_cooking.
#
# Linking named domains instead of the compatibility aggregate is the point of
# the decomposition: a sample waits only on the optional targets it uses, and a
# sample with no optional targets gets no simdjson, sockets, or VFX toolkit.
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_ProceduralAnimationSample zahlen_animation zahlen_gltf zahlen_terrain zahlen_camera zahlen_character_controller)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_GlTFInspectorSample zahlen_gltf)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_RemoteGLBSample zahlen_gltf zahlen_remote_asset zahlen_github)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_CDNMapSample zahlen_gltf zahlen_cdn zahlen_freecam)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_DeviceLostRecoverySample zahlen_freecam)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_FidelityHarness zahlen_gltf zahlen_serialization zahlen_asset_cooking)
set(ZHLN_SAMPLE_OPTIONAL_TARGETS_BSPMurderDronesSample zahlen_bsp zahlen_freecam zahlen_interaction)

foreach(SAMPLE_SRC IN LISTS SAMPLE_SOURCES)
    get_filename_component(SAMPLE_NAME ${SAMPLE_SRC} NAME_WE)

    # A sample whose required optional targets were not built cannot compile -- its import
    # would have no BMI to bind to -- so it is skipped rather than failed. The
    # same rule that used to guard ProceduralAnimationSample by name now covers
    # every sample with an optional-layer dependency.
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

    # No WIN32 keyword: on Windows, these are console applications by default.
    add_executable(${SAMPLE_NAME} ${SAMPLE_SRC})

    set_target_properties(${SAMPLE_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES ON
    )

    target_link_libraries(${SAMPLE_NAME} PRIVATE
        zahlen_engine
        Jolt
        zahlen_warnings
    )

    # Link only the optional targets this sample uses (e.g. ZHLN.Locomotion and
    # Character::Install). Each carries its public include root and any module
    # interfaces, so the sample can resolve both <glTF/...> and imports above.
    foreach(ZHLN_SAMPLE_OPTIONAL_TARGET IN LISTS ZHLN_SAMPLE_OPTIONAL_TARGETS_${SAMPLE_NAME})
        target_link_libraries(${SAMPLE_NAME} PRIVATE ${ZHLN_SAMPLE_OPTIONAL_TARGET})
    endforeach()

    target_compile_definitions(${SAMPLE_NAME} PRIVATE
        JPH_DOUBLE_PRECISION
    )

    # The composition-root default font: every sample boots an Engine, and each
    # one installs the baked-font loader with the vendored JetBrains Mono NF bake
    # (see app/main.cpp and Fonts::VendoredDefaultFontSource) so its text renders
    # with a real font instead of core's embedded 8x8 fallback.
    #
    # Not listed in ZHLN_SAMPLE_OPTIONAL_TARGETS_*: those are required domains, and a
    # missing target there skips the sample. This link is optional by design --
    # without the font domain, ZHLN_HAS_FONTS stays undefined and the sample
    # keeps building and running on the embedded bake.
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

    if(USE_SANITIZERS AND WIN32 AND ASAN_DLL)
        add_custom_command(TARGET ${SAMPLE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASAN_DLL}" "$<TARGET_FILE_DIR:${SAMPLE_NAME}>"
            COMMENT "Copying ASan Runtime for ${SAMPLE_NAME}..."
        )
    endif()

    # A sample links the engine's DLL and Jolt's, and is built into a directory
    # beside neither of them. Same staging as the ASan runtime above, and for the
    # same reason: a person runs this one.
    zahlen_add_runtime_dlls(${SAMPLE_NAME})
endforeach()
