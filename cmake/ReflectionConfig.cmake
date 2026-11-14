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
include(CompilerFlagProbe)
zhln_check_cxx_compiler_flag("-freflection" COMPILER_HAS_REFLECTION)

if(COMPILER_HAS_REFLECTION)
    message(STATUS "C++26 Static Reflection supported via -freflection")
    # Bloomberg Clang gates P3096's std::meta::parameters_of behind a second
    # flag; -freflection alone supplies only P2996. GCC exposes it with
    # -freflection, so the extra flag is Clang-specific.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        zhln_check_cxx_compiler_flag("-fconstexpr-steps=33554432" COMPILER_HAS_CONSTEXPR_STEPS_FLAG)
        zhln_check_cxx_compiler_flag("-freflection -fparameter-reflection" COMPILER_HAS_PARAMETER_REFLECTION_FLAG)
        if(NOT COMPILER_HAS_PARAMETER_REFLECTION_FLAG)
            # Newer forks also provide a unified flag for companion proposals.
            zhln_check_cxx_compiler_flag("-freflection -freflection-latest" COMPILER_HAS_REFLECTION_LATEST_FLAG)
            if(NOT COMPILER_HAS_REFLECTION_LATEST_FLAG)
                message(FATAL_ERROR "This Clang supports -freflection but not P3096 parameter reflection (-fparameter-reflection or -freflection-latest).")
            endif()
        endif()
    endif()
else()
    if(NOT ZHLN_HAS_PYTHON)
        message(FATAL_ERROR
            "C++26 static reflection (-freflection) is not supported by ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}), "
            "and Python 3 was not found (ZHLN_HAS_PYTHON=OFF) to run the fallback reflection transpiler "
            "(scripts/transpile_reflection.py). Configure with a -freflection compiler (GCC 16+ or clang-p2996) "
            "or install Python 3.")
    endif()
    message(STATUS "C++26 Static Reflection not supported by current compiler. Using generated script for source code flattening.")
endif()

# --- FALLBACK TRANSPILER (compilers without -freflection) ---
set(ZHLN_TRANSPILER_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/transpile_reflection.py")

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
# --- the custom libc++ ------------------------------------------------------
#
# One problem, one place. Everything about the branch toolchain's C++ runtime is
# decided here, once, and every target in the tree inherits it -- nothing per
# target, and nothing in a profile script.
#
# Why it cannot be left to -stdlib=libc++ alone: that flag selects the *headers*,
# which clang resolves through its resource directory ($RESOURCE_DIR/include/c++/v1).
# Where -lc++, -lc++abi and -lunwind resolve is the linker's search path, and the
# Darwin driver contributes no -L for a runtime installed under <prefix>/lib --
# Apple's model is that libc++ is a platform library in the SDK. So the headers come
# from the branch and the images come from the SDK unless something says otherwise.
# That mismatch is not a missing-symbol problem in the usual sense: <meta> and
# std::define_static_array exist only in the branch headers, so the SDK's libc++
# cannot be used at all, and an image older than the headers leaves exactly the
# functions newly added to it (std::__hash_memory, std::__from_chars_floating_point)
# undefined while everything else in the same library links fine.
#
# Why it cannot be per target: an LLVM tree's libc++ does not re-export libc++abi the
# way the platform's umbrella stub does, and it carries @rpath load commands for
# libc++abi and libunwind. A target that names -lc++ without the directory therefore
# links nothing of libc++abi, and a process in which one C++ image is on the platform
# runtime and another on the branch one hands a std::locale across a boundary where
# the two disagree about its layout -- which aborts inside malloc. Both are the same
# mistake: the runtime has to be chosen once, for the whole build.
#
# The three mechanisms below are directory-scoped, so every target created after this
# point -- including extern/JoltPhysics, whose CMakeLists.txt knows nothing about any
# of this -- inherits them:
#
#   add_compile_options  -stdlib=libc++                     headers, every TU
#   link_directories     -L<runtime dir>                    link-time, every -l
#   add_link_options     -stdlib=libc++, -Wl,-rpath,<dir>   the driver's own choice of
#                                                           runtime, and the load path
#
# Inert unless ZHLN_USE_CUSTOM_LIBCXX is ON. Sets these cache variables for reference:
# LLVM_BLOOMBERG_ROOT, LLVM_BLOOMBERG_BUILD, ZHLN_LIBCXX_RUNTIME_DIR.
function(zahlen_configure_custom_libcxx)
    if(NOT ZHLN_USE_CUSTOM_LIBCXX OR NOT ZHLN_HAS_STDLIB_LIBCXX)
        return()
    endif()

    get_filename_component(_default_root "${CMAKE_SOURCE_DIR}/../llvm-p2996" ABSOLUTE)
    set(LLVM_BLOOMBERG_ROOT "${_default_root}" CACHE PATH "Path to Bloomberg LLVM monorepo")
    set(LLVM_BLOOMBERG_BUILD "${LLVM_BLOOMBERG_ROOT}/build" CACHE PATH "Path to Bloomberg LLVM build directory")

    if(NOT EXISTS "${LLVM_BLOOMBERG_ROOT}")
        message(FATAL_ERROR
            "ZHLN_USE_CUSTOM_LIBCXX is ON, but LLVM root was not found at: ${LLVM_BLOOMBERG_ROOT}\n"
            "Pass -DLLVM_BLOOMBERG_ROOT=/path/to/llvm-p2996 to configure manually.")
    endif()

    # lib, lib64, and one level down: lib/<target-triple>/ -- an LLVM build tree puts
    # its runtimes in whichever of those it was told to use.
    file(GLOB _subdirs LIST_DIRECTORIES true
        "${LLVM_BLOOMBERG_BUILD}/lib" "${LLVM_BLOOMBERG_BUILD}/lib64"
        "${LLVM_BLOOMBERG_BUILD}/lib/*" "${LLVM_BLOOMBERG_BUILD}/lib64/*")
    set(_dirs "${LLVM_BLOOMBERG_BUILD}/lib" "${LLVM_BLOOMBERG_BUILD}/lib64")
    foreach(_d IN LISTS _subdirs)
        if(IS_DIRECTORY "${_d}")
            list(APPEND _dirs "${_d}")
        endif()
    endforeach()

    set(_runtime_dir "")
    foreach(_d IN LISTS _dirs)
        if(EXISTS "${_d}/libc++.dylib" OR EXISTS "${_d}/libc++.so")
            set(_runtime_dir "${_d}")
            break()
        endif()
    endforeach()

    if(NOT _runtime_dir)
        message(FATAL_ERROR
            "ZHLN_USE_CUSTOM_LIBCXX is ON, but no directory under ${LLVM_BLOOMBERG_BUILD} holds "
            "libc++.dylib or libc++.so. This build cannot use the platform libc++: the reflection "
            "headers need <meta> and std::define_static_array, which exist only in the branch's "
            "libc++.\nRebuild the runtimes (ninja libcxx libcxxabi libunwind in "
            "${LLVM_BLOOMBERG_BUILD}), or point LLVM_BLOOMBERG_BUILD at a tree that has them.")
    endif()
    set(ZHLN_LIBCXX_RUNTIME_DIR "${_runtime_dir}" CACHE INTERNAL
        "Directory holding the C++ runtime this build links against" FORCE)

    # A libc++ built from source does not re-export libc++abi, so -lc++abi has to find a
    # real image in the same directory. Without one, only libc++abi's symbols are
    # undefined -- half a runtime missing is the signature of a search path.
    if(NOT EXISTS "${_runtime_dir}/libc++abi.dylib" AND NOT EXISTS "${_runtime_dir}/libc++abi.so")
        message(FATAL_ERROR
            "${_runtime_dir} holds libc++ but no libc++abi. A libc++ built from source does not "
            "re-export libc++abi the way the platform's umbrella stub does, so every libc++abi "
            "symbol would come out undefined. Rebuild libcxxabi into the same directory.")
    endif()

    # Inspect actual shared-library exports, not a Linux linker script.
    find_program(ZHLN_NM_EXECUTABLE NAMES llvm-nm nm)
    if(APPLE)
        set(_image "${_runtime_dir}/libc++.dylib")
        set(_nm_args -gU)
    else()
        set(_image "${_runtime_dir}/libc++.so.1")
        set(_nm_args -D --defined-only)
    endif()
    if(EXISTS "${_image}" AND ZHLN_NM_EXECUTABLE)
        execute_process(
            COMMAND "${ZHLN_NM_EXECUTABLE}" ${_nm_args} "${_image}"
            RESULT_VARIABLE _nm_result
            OUTPUT_VARIABLE _syms ERROR_VARIABLE _nm_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT "${_nm_result}" STREQUAL "0")
            message(FATAL_ERROR
                "Could not inspect runtime exports in ${_image}: ${_nm_error}. "
                "This is an inspection failure, not proof of a header/runtime mismatch.")
        endif()
        if(NOT _syms MATCHES "__hash_memory")
            message(FATAL_ERROR
                "${_image} was inspected successfully but exports no __hash_memory. "
                "Check that the selected shared runtime matches the branch headers.")
        endif()
    else()
        message(WARNING
            "Skipping libc++ export check: actual shared image ${_image} or nm is unavailable. "
            "The link step will validate runtime symbols.")
    endif()

    # One shared branch runtime for all targets created below this directory.
    # Do not embed libc++ archives independently in the engine and executables:
    # exported functions can bind across images while locale state stays local.
    if(APPLE)
        foreach(_library IN ITEMS libc++.dylib libc++abi.dylib)
            if(NOT EXISTS "${_runtime_dir}/${_library}")
                message(FATAL_ERROR
                    "Shared branch runtime required: ${_runtime_dir}/${_library} is missing. "
                    "Rebuild the toolchain shared runtimes; static fallback is not safe here.")
            endif()
        endforeach()
        # The namespace must be generated by the runtime build, not supplied as
        # a consumer-only define. It separates branch symbols from Apple's __1.
        set(_config_site "${LLVM_BLOOMBERG_BUILD}/include/c++/v1/__config_site")
        if(NOT EXISTS "${_config_site}")
            message(FATAL_ERROR "Missing branch runtime configuration: ${_config_site}")
        endif()
        file(READ "${_config_site}" _runtime_config)
        if(NOT _runtime_config MATCHES "#[ \t]*define[ \t]+_LIBCPP_ABI_NAMESPACE[ \t]+__p2996([^A-Za-z0-9_]|$)")
            message(FATAL_ERROR
                "Rebuild branch libc++ with -DLIBCXX_ABI_NAMESPACE=__p2996. "
                "Its generated headers must match the rebuilt shared library.")
        endif()
    endif()

    # OBJCXX is in this list because on Apple the root enables OBJCXX for Jolt's
    # Metal backend, and a language left out here compiles against the SDK's
    # libc++/libc++abi headers while the C++ half of the same process links the
    # branch runtime -- the standard-library-ABI mismatch that aborts inside
    # malloc/std::locale. It is one build either way, so it is one runtime.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>"
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-stdlib=libc++>")
    # Link-time search and run-time lookup are separate requirements.
    link_directories(BEFORE "${_runtime_dir}")
    add_link_options(
        "$<$<LINK_LANGUAGE:CXX>:-stdlib=libc++>"
        "$<$<LINK_LANGUAGE:OBJCXX>:-stdlib=libc++>"
        "-Wl,-rpath,${_runtime_dir}")
    link_libraries(c++ c++abi)
    if(NOT APPLE)
        link_libraries(unwind)
    endif()
    message(STATUS "libc++: shared branch runtime at ${_runtime_dir}; no per-image runtime archives")
endfunction()

# Kept as a no-op so existing call sites (zahlen_enable_reflection, and any target that
# called it directly) keep working. Everything it used to do is now directory-scoped by
# zahlen_configure_custom_libcxx() above, which is the point: there is nothing left to
# decide per target, and a second place deciding it is how targets ended up on different
# runtimes in the first place.
function(zahlen_use_custom_libcxx TARGET_NAME)
endfunction()

function(zahlen_enable_reflection TARGET_NAME)
    if(COMPILER_HAS_REFLECTION)
        # These flags are one set, and they have to travel as usage
        # requirements rather than as flags only the target itself compiles
        # with. Since 4.4 CMake gives every consumer whose compile options
        # differ from the provider's a synthetic BMI target
        # ("<provider>@synth_<n>") that rebuilds the provider's module units
        # with the consumer's own plus transitive options and re-applies the
        # provider's preprocessor options (defines, -I, -D) on top. A PRIVATE
        # flag is dropped there: zahlen_engine's -fparameter-reflection reached
        # such a target while -freflection did not, and the scan of
        # modules/*.cppm died with "cannot specify '-fparameter-reflection'
        # without '-freflection'". Public keeps the set together wherever the
        # module units get built, and a consumer that adds nothing of its own
        # now matches this target's options outright, so no second BMI set is
        # generated for it at all.
        target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
        if(COMPILER_HAS_PARAMETER_REFLECTION_FLAG)
            # CallableInspector is public API: consumers compiling its
            # templates need the same P3096 flag as Zahlen itself.
            target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-fparameter-reflection>")
        elseif(COMPILER_HAS_REFLECTION_LATEST_FLAG)
            target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-freflection-latest>")
        endif()
        if(COMPILER_HAS_CONSTEXPR_STEPS_FLAG)
            # Match GCC's default -fconstexpr-ops-limit=33554432 (2^25); Clang's
            # pre-reflection 2^20 default can saturate on large P2996 schemas.
            target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-fconstexpr-steps=33554432>")
        endif()
        if(ZHLN_HAS_ANNOTATION_ATTRIBUTES)
            # The same set has one more member: the annotation syntax only
            # appears where __cpp_impl_reflection is defined, and a synthetic
            # target is handed that define back while losing a PRIVATE flag.
            target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-fannotation-attributes>")
        endif()
        # PUBLIC, not PRIVATE, and for the same reason the flags above are.
        #
        # This define is not a duplicate of the compiler's own: GCC 16 defines
        # __cpp_impl_reflection from -freflection alone, while clang-p2996 forks
        # do not, and every reflection header in this tree keys off it
        # (ZHLN_REFLECTION_AVAILABLE in Core/Reflection/Core.hpp). A consumer
        # that receives -freflection but not the define compiles a *different*
        # ReflectionAvailable, so LayoutHash<T>() takes its sizeof/alignof-only
        # path and every reflected type hashes to something the engine will
        # never compute -- a silent wrong-offset read, not a link error.
        #
        # The synthetic BMI target described above is the second reason: it
        # re-applies a provider's preprocessor options but drops a PRIVATE
        # compile *definition*, so a PRIVATE define here would leave the
        # consumer's module units on the other side of the #if.
        #
        # Where the compiler already supplies the value this is redundant, not
        # harmful: a command-line -D is accepted alongside a predefined macro
        # with the same name and value (verified with GCC 16.2: no diagnostic
        # under this project's -Wall -Wextra -Wpedantic -Wundef -Werror set).
        target_compile_definitions(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:__cpp_impl_reflection=202603L>")
    endif()
    zahlen_use_custom_libcxx(${TARGET_NAME})
endfunction()
