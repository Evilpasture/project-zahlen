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

foreach(SAMPLE_SRC IN LISTS SAMPLE_SOURCES)
    get_filename_component(SAMPLE_NAME ${SAMPLE_SRC} NAME_WE)

    add_executable(${SAMPLE_NAME} ${SAMPLE_SRC})

    set_target_properties(${SAMPLE_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES ON
    )

    target_link_libraries(${SAMPLE_NAME} PRIVATE
        zahlen_engine
        imgui
        Jolt
        zahlen_warnings
    )

    # Link the extras toolkit library to import modules (e.g., ZHLN.Locomotion)
    if(TARGET zahlen_extras)
        target_link_libraries(${SAMPLE_NAME} PRIVATE zahlen_extras)
    endif()

    target_compile_definitions(${SAMPLE_NAME} PRIVATE
        JPH_DOUBLE_PRECISION
    )

    if(NOT MSVC)
        target_compile_options(${SAMPLE_NAME} PRIVATE
            $<$<AND:$<CXX_COMPILER_ID:GNU>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions -fno-rtti>
        )
        if(WIN32)
            target_link_options(${SAMPLE_NAME} PRIVATE -Xlinker /subsystem:console)
        endif()
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
