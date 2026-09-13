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
#                              ZHLN.ProceduralAnimation (zahlen_animation), and
#                              includes <glTF/GLTFImporter.hpp> (zahlen_gltf) to
#                              load the rig it animates.
#   GlTFInspectorSample        imports ZHLN.glTF (zahlen_gltf).
#
# Linking the named domains instead of the zahlen_extras aggregate is the point
# of the decomposition: a sample's build now waits on the module interfaces it
# actually imports and on nothing else, and a sample that wants no extras does
# not get a link to simdjson, sockets or the VFX toolkits.
set(ZHLN_SAMPLE_EXTRAS_ProceduralAnimationSample zahlen_animation zahlen_gltf)
set(ZHLN_SAMPLE_EXTRAS_GlTFInspectorSample zahlen_gltf)

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
