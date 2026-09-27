# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/ReflectionConfig.cmake
#
# C++26 static reflection (-freflection / P2996): what the compiler supports,
# the fallback transpiler for compilers that do not, and the per-target flag
# helpers the root CMakeLists applies. Reflection is not a per-target feature --
# it changes struct layout -- so the helpers are deliberately centralised here
# and applied from the root's REFLECTION FLAGS PROPAGATION section.

# --- DETECT C++26 LANGUAGE FEATURE FLAGS ---
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-freflection" COMPILER_HAS_REFLECTION)

if(COMPILER_HAS_REFLECTION)
    message(STATUS "C++26 Static Reflection supported via -freflection")
else()
    message(STATUS "C++26 Static Reflection not supported by current compiler. Using generated script for source code flattening.")
endif()

# --- FALLBACK TRANSPILER (compilers without -freflection) ---
set(ZHLN_TRANSPILER_SCRIPT "${CMAKE_SOURCE_DIR}/tools/transpile_reflection.py")

function(zahlen_transpile_sources OUT_SOURCES_VAR)
    if(COMPILER_HAS_REFLECTION)
        # Native support: Return sources untouched
        set(${OUT_SOURCES_VAR} ${ARGN} PARENT_SCOPE)
        return()
    endif()

    set(PROCESSED_SRCS "")
    set(ORIG_SOURCE_DIRS "")

    foreach(SRC IN LISTS ARGN)
        get_filename_component(ABS_SRC "${SRC}" ABSOLUTE)
        get_filename_component(ABS_DIR "${ABS_SRC}" DIRECTORY)
        list(APPEND ORIG_SOURCE_DIRS "${ABS_DIR}")

        file(RELATIVE_PATH REL_SRC "${CMAKE_SOURCE_DIR}" "${ABS_SRC}")
        set(OUT_SRC "${CMAKE_BINARY_DIR}/transpiled/${REL_SRC}")
        get_filename_component(OUT_DIR "${OUT_SRC}" DIRECTORY)

        file(MAKE_DIRECTORY "${OUT_DIR}")

        # At build time, run the Python transpiler using the compilation database
        add_custom_command(
            OUTPUT "${OUT_SRC}"
            COMMAND Python3::Interpreter "${ZHLN_TRANSPILER_SCRIPT}"
                    --input "${ABS_SRC}"
                    --output "${OUT_SRC}"
                    --compdb "${CMAKE_BINARY_DIR}"
            DEPENDS "${ABS_SRC}" "${ZHLN_TRANSPILER_SCRIPT}"
            COMMENT "Flattening Reflection in ${REL_SRC}..."
            VERBATIM
        )
        list(APPEND PROCESSED_SRCS "${OUT_SRC}")
    endforeach()

    # Register original source directories so transpiled files in build/ can locate relative headers
    list(REMOVE_DUPLICATES ORIG_SOURCE_DIRS)
    include_directories(${ORIG_SOURCE_DIRS})

    set(${OUT_SOURCES_VAR} ${PROCESSED_SRCS} PARENT_SCOPE)
endfunction()

# --- PER-TARGET REFLECTION FLAGS & CUSTOM LIBC++ ---
function(zahlen_use_custom_libcxx TARGET_NAME)
    option(ZHLN_USE_CUSTOM_LIBCXX "Link against Bloomberg custom libc++" OFF)

    # Only apply custom LLVM libc++ when explicitly enabled and the compiler
    # actually speaks -stdlib=libc++ (feature-probed at the root: GCC refuses
    # the flag, so the probe keeps this function Clang-shaped without naming
    # any compiler).
    if(NOT ZHLN_USE_CUSTOM_LIBCXX OR NOT ZHLN_HAS_STDLIB_LIBCXX)
        return()
    endif()

    get_filename_component(_DEFAULT_LLVM_ROOT "${CMAKE_SOURCE_DIR}/../llvm-p2996" ABSOLUTE)
    set(LLVM_BLOOMBERG_ROOT "${_DEFAULT_LLVM_ROOT}" CACHE PATH "Path to Bloomberg LLVM monorepo")
    set(LLVM_BLOOMBERG_BUILD "${LLVM_BLOOMBERG_ROOT}/build" CACHE PATH "Path to Bloomberg LLVM build directory")

    if(NOT EXISTS "${LLVM_BLOOMBERG_ROOT}")
        message(FATAL_ERROR "ZHLN_USE_CUSTOM_LIBCXX is ON, but LLVM root was not found at: ${LLVM_BLOOMBERG_ROOT}\n"
                           "Pass -DLLVM_BLOOMBERG_ROOT=/path/to/llvm-p2996 to configure manually.")
    endif()

    file(GLOB _LLVM_LIB_SUBDIRS LIST_DIRECTORIES true "${LLVM_BLOOMBERG_BUILD}/lib/*")
    set(_ALL_LLVM_LIB_DIRS "${LLVM_BLOOMBERG_BUILD}/lib")
    foreach(_DIR IN LISTS _LLVM_LIB_SUBDIRS)
        if(IS_DIRECTORY "${_DIR}")
            list(APPEND _ALL_LLVM_LIB_DIRS "${_DIR}")
        endif()
    endforeach()

    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>
    )

    foreach(_LIB_DIR IN LISTS _ALL_LLVM_LIB_DIRS)
        target_link_options(${TARGET_NAME} PRIVATE
            $<$<PLATFORM_ID:Linux>:-L${_LIB_DIR}>
            $<$<PLATFORM_ID:Linux>:-Wl,-rpath,${_LIB_DIR}>
        )
    endforeach()

    target_link_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        c++
        c++abi
        unwind
    )
endfunction()

function(zahlen_enable_reflection TARGET_NAME)
    if(COMPILER_HAS_REFLECTION)
        target_compile_options(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
        if(ZHLN_HAS_ANNOTATION_ATTRIBUTES)
            target_compile_options(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fannotation-attributes>")
        endif()
        target_compile_definitions(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:__cpp_impl_reflection=202603L>")
    endif()
    zahlen_use_custom_libcxx(${TARGET_NAME})
endfunction()
