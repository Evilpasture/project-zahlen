# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(GLOBAL)

# --- DISCOVER OR BUILD RUST ECS COMPILER (ECSC) ---
set(ZHLN_ECSC_SEARCH_PATHS
    "${CMAKE_SOURCE_DIR}/tools/bin"
    "${CMAKE_SOURCE_DIR}/tools/ecsc/target/release"
)

find_program(ECSC_BIN NAMES ecsc ecsc.exe HINTS ${ZHLN_ECSC_SEARCH_PATHS})

if(NOT ECSC_BIN)
    find_program(CARGO_BIN NAMES cargo)
    if(CARGO_BIN)
        message(STATUS "ecsc binary not found. Compiling ecsc via Cargo...")
        execute_process(
            COMMAND ${CARGO_BIN} build --release
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/ecsc"
            RESULT_VARIABLE CARGO_RES
        )
        if(NOT CARGO_RES EQUAL 0)
            message(FATAL_ERROR "Failed to build ecsc via Cargo. Check Cargo output above.")
        endif()

        find_program(ECSC_BIN NAMES ecsc ecsc.exe HINTS ${ZHLN_ECSC_SEARCH_PATHS} NO_DEFAULT_PATH)
    else()
        message(FATAL_ERROR "Neither prebuilt 'ecsc' binary nor 'cargo' toolchain was found in PATH.")
    endif()
endif()

if(ECSC_BIN)
    message(STATUS "Using ECS DSL Compiler: ${ECSC_BIN}")
endif()

# --- FUNCTION: COMPILE .ECS FILES TO C++20/26 MODULES ---
function(zahlen_compile_ecs_modules TARGET_NAME)
    if(NOT ECSC_BIN)
        message(FATAL_ERROR "zahlen_compile_ecs_modules called, but 'ecsc' binary is unavailable.")
    endif()

    set(GENERATED_MODULE_FILES "")
    set(GENERATED_CPP_FILES "")
    set(OUT_DIR "${CMAKE_BINARY_DIR}/generated_ecs")

    file(MAKE_DIRECTORY "${OUT_DIR}")

    foreach(ECS_SRC IN LISTS ARGN)
        get_filename_component(ABS_SRC "${ECS_SRC}" ABSOLUTE)
        get_filename_component(FILE_NAME_WE "${ABS_SRC}" NAME_WLE)

        set(OUT_CPPM "${OUT_DIR}/${FILE_NAME_WE}.cppm")
        set(OUT_CPP  "${OUT_DIR}/${FILE_NAME_WE}.cpp")

        # 1. Instant Configure-Time Generation (Guarantees clangd/LSP index availability)
        execute_process(
            COMMAND "${ECSC_BIN}" compile
                    -i "${ABS_SRC}"
                    --output-cppm "${OUT_CPPM}"
                    --output-cpp "${OUT_CPP}"
            RESULT_VARIABLE ECSC_RES
        )

        if(NOT ECSC_RES EQUAL 0)
            message(WARNING "ecsc failed to compile ${FILE_NAME_WE}.ecs during configuration stage.")
        endif()

        # 2. Build-Time Target (Triggers Ninja rebuilds whenever .ecs files change)
        add_custom_command(
            OUTPUT "${OUT_CPPM}" "${OUT_CPP}"
            COMMAND "${ECSC_BIN}" compile
                    -i "${ABS_SRC}"
                    --output-cppm "${OUT_CPPM}"
                    --output-cpp "${OUT_CPP}"
            DEPENDS "${ABS_SRC}" "${ECSC_BIN}"
            COMMENT "ecsc (Rust): Compiling DSL ${FILE_NAME_WE}.ecs -> C++20/26 Module..."
            VERBATIM
        )

        list(APPEND GENERATED_MODULE_FILES "${OUT_CPPM}")
        list(APPEND GENERATED_CPP_FILES "${OUT_CPP}")
    endforeach()

    # Register generated C++ module interface files (.cppm) with CMake 3.28+ CXX_MODULES
    target_sources(${TARGET_NAME}
        PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS "${OUT_DIR}"
            FILES ${GENERATED_MODULE_FILES}
        PRIVATE
            ${GENERATED_CPP_FILES}
    )
endfunction()
