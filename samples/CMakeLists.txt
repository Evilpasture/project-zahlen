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

# Which extras domains each sample consumes, by sample name. A sample that
# appears in none of these lists links no extras at all.
#
#   ProceduralAnimationSample  imports ZHLN.Locomotion and
#                              ZHLN.ProceduralAnimation (zahlen_animation),
#                              includes <glTF/GLTFImporter.hpp> (zahlen_gltf)
#                              to load the rig it animates, builds its
#                              ground with <Terrain/TerrainFactory.hpp>
#                              (zahlen_terrain), and drives its third-person
#                              view through the extras camera rig
#                              (<Camera/TargetCamera.hpp>, zahlen_camera).
#   GlTFInspectorSample        imports ZHLN.glTF (zahlen_gltf).
#   RemoteGLBSample            fetches through ZHLN::Remote (zahlen_remote_asset
#                              over extras/HTTP) and imports with ZHLN.glTF
#                              (zahlen_gltf); its model-list crawl goes through
#                              ZHLN::GitHub (zahlen_github, over extras/HTTP
#                              and extras/json).
#   FidelityHarness            headless glTF-Render-Fidelity runner: imports
#                              with ZHLN.glTF (zahlen_gltf) and reads the
#                              Khronos scenario JSONs with ZHLN::ReflectJSON
#                              (zahlen_serialization -- extras/json).
#
# Linking the named domains instead of the zahlen_extras aggregate is the point
# of the decomposition: a sample's build now waits on the module interfaces it
# actually imports and on nothing else, and a sample that wants no extras does
# not get a link to simdjson, sockets or the VFX toolkits.
set(ZHLN_SAMPLE_EXTRAS_ProceduralAnimationSample zahlen_animation zahlen_gltf zahlen_terrain zahlen_camera)
set(ZHLN_SAMPLE_EXTRAS_GlTFInspectorSample zahlen_gltf)
set(ZHLN_SAMPLE_EXTRAS_RemoteGLBSample zahlen_gltf zahlen_remote_asset zahlen_github)
set(ZHLN_SAMPLE_EXTRAS_FidelityHarness zahlen_gltf zahlen_serialization)

foreach(SAMPLE_SRC IN LISTS SAMPLE_SOURCES)
    get_filename_component(SAMPLE_NAME ${SAMPLE_SRC} NAME_WE)

    # A sample whose extras domains were not built cannot compile -- its import
    # would have no BMI to bind to -- so it is skipped rather than failed. The
    # same rule that used to guard ProceduralAnimationSample by name now covers
    # every sample with an extras dependency.
    set(ZHLN_SAMPLE_MISSING_EXTRAS "")
    foreach(ZHLN_SAMPLE_EXTRA IN LISTS ZHLN_SAMPLE_EXTRAS_${SAMPLE_NAME})
        if(NOT TARGET ${ZHLN_SAMPLE_EXTRA})
            list(APPEND ZHLN_SAMPLE_MISSING_EXTRAS ${ZHLN_SAMPLE_EXTRA})
        endif()
    endforeach()
    if(ZHLN_SAMPLE_MISSING_EXTRAS)
        message(STATUS "Skipping sample ${SAMPLE_NAME}: extras target(s) not built: ${ZHLN_SAMPLE_MISSING_EXTRAS}")
        continue()
    endif()

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

    # Link the extras domains this sample imports (e.g. ZHLN.Locomotion). Each
    # one carries the extras include root and its module interfaces publicly, so
    # this is all the sample needs to resolve <glTF/...> and the imports above.
    foreach(ZHLN_SAMPLE_EXTRA IN LISTS ZHLN_SAMPLE_EXTRAS_${SAMPLE_NAME})
        target_link_libraries(${SAMPLE_NAME} PRIVATE ${ZHLN_SAMPLE_EXTRA})
    endforeach()

    target_compile_definitions(${SAMPLE_NAME} PRIVATE
        JPH_DOUBLE_PRECISION
    )

    # The composition-root default font: every sample boots an Engine, and each
    # one installs the baked-font loader with the vendored JetBrains Mono NF bake
    # (see app/main.cpp and Fonts::VendoredDefaultFontSource) so its text renders
    # with a real font instead of core's embedded 8x8 fallback.
    #
    # Not listed in ZHLN_SAMPLE_EXTRAS_*: that map is for domains a sample's
    # source *imports*, and a missing entry there skips the sample. This link is
    # optional by design -- with ZHLN_BUILD_EXTRAS=OFF there is no font domain to
    # install, ZHLN_HAS_FONTS stays undefined, and the sample keeps building and
    # running on the embedded bake.
    if(TARGET zahlen_fonts)
        target_link_libraries(${SAMPLE_NAME} PRIVATE zahlen_fonts)
        target_compile_definitions(${SAMPLE_NAME} PRIVATE ZHLN_HAS_FONTS=1)
    endif()

    if(NOT MSVC AND WIN32)
        target_link_options(${SAMPLE_NAME} PRIVATE -Xlinker /subsystem:console)
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
endforeach()
