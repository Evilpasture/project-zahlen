# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Declare a dynamically loadable COM::IAppLayer shared library linked against
# libzahlen_engine, exporting CreateAppLayerRaw and its ABI witness.
function(zahlen_add_plugin TARGET_NAME PLUGIN_NAME)
    add_library(${TARGET_NAME} SHARED ${ARGN})
    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        OUTPUT_NAME "zahlen_plugin_${PLUGIN_NAME}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${TARGET_NAME} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib"
            RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin"
        )
    endforeach()
    target_link_libraries(${TARGET_NAME} PRIVATE zahlen_engine zahlen_warnings)
    if(UNIX AND NOT APPLE)
        set(_zhln_plugin_map "${PROJECT_SOURCE_DIR}/cmake/zahlen_plugin.map")
        target_link_options(${TARGET_NAME} PRIVATE
            "LINKER:--exclude-libs,ALL"
            "LINKER:--version-script=${_zhln_plugin_map}"
        )
        set_property(TARGET ${TARGET_NAME} APPEND PROPERTY LINK_DEPENDS "${_zhln_plugin_map}")
    elseif(MINGW)
        target_link_options(${TARGET_NAME} PRIVATE "LINKER:--exclude-libs,ALL")
    elseif(APPLE)
        # Mach-O prepends an extra leading underscore to symbol table entries:
        # dlsym takes the Itanium name ("_Z..."), while ld64 -exported_symbol
        # takes the Mach-O symbol ("__Z...").
        target_link_options(${TARGET_NAME} PRIVATE "LINKER:-exported_symbol,__ZN4ZHLN3COM17CreateAppLayerRaw*"
            "LINKER:-exported_symbol,__ZN4ZHLN3COM8Contract*4SealEv")
    endif()
    if(COMMAND zahlen_enable_reflection)
        zahlen_enable_reflection(${TARGET_NAME})
    endif()
    if(USE_SANITIZERS AND COMMAND zahlen_install_sanitizer_runtime)
        zahlen_install_sanitizer_runtime(${TARGET_NAME})
    endif()
endfunction()
