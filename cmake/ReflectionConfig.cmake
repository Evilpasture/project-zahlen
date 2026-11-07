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

    # Which libc++ this target links.
    #
    # The rpath above is a load-time search path: it tells the loader where to
    # find the library when the binary runs, and it has no effect whatsoever on
    # what the linker resolves -lc++ to. Link-time resolution is the driver's
    # search order, and for a toolchain built from source that order does not
    # necessarily start in the toolchain. A build can therefore compile against
    # one libc++ and link another, which is not a missing-symbol problem: the
    # headers declare "this new function lives in the dylib" (extern template in
    # __charconv, _LIBCPP_EXPORTED_FROM_ABI on __hash_memory) and an older image
    # simply has nothing to resolve it to, while everything else in the same
    # library links fine.
    #
    # Naming the file in target_link_libraries does not fix that: CMake
    # normalizes an absolute lib<name>.dylib path back into -L<dir> -l<name>.
    # The -L is the part that decides, so the -L is what goes on the line, scoped
    # to these targets rather than to CMAKE_CXX_FLAGS (a global -L changes the
    # lookup for every library in the build, which is how a scoped problem became
    # a whole-build one before).
    #
    # libunwind stays a bare name on Linux only: there an LLVM tree keeps
    # libunwind.so beside libc++.so and the loader must be told which to take. On
    # Darwin the _Unwind_* entry points are in libSystem, which every Mach-O links
    # regardless, so there is no separate image to name -- and ld64.lld will not
    # find an SDK stub by bare name, so an explicit -lunwind there fails outright.
    set(_ZHLN_RUNTIME_DIR "")
    foreach(_ZHLN_LIB_DIR IN LISTS _ALL_LLVM_LIB_DIRS)
        if(EXISTS "${_ZHLN_LIB_DIR}/libc++.dylib" OR EXISTS "${_ZHLN_LIB_DIR}/libc++.so")
            set(_ZHLN_RUNTIME_DIR "${_ZHLN_LIB_DIR}")
            break()
        endif()
    endforeach()

    if(_ZHLN_RUNTIME_DIR)
        # PUBLIC, not PRIVATE. A consumer of this target resolves -lc++ on its own
        # link line, so an -L that stopped at the provider would let the engine and
        # the executable that links it end up on different libc++ images -- the
        # same two-version build, one level up. The rpath travels with it because
        # the daemon and the host tools are run out of the build tree.
        target_link_options(${TARGET_NAME} PUBLIC
            "-L${_ZHLN_RUNTIME_DIR}"
            "-Wl,-rpath,${_ZHLN_RUNTIME_DIR}"
        )
    endif()

    target_link_libraries(${TARGET_NAME} PRIVATE
        c++
        c++abi
    )
    if(NOT APPLE)
        target_link_libraries(${TARGET_NAME} PRIVATE unwind)
    endif()

    # The headers and the image have to come from the same build of libc++, and
    # nothing in the link line says so -- so check it, once per configure, on the
    # file the -L above will resolve to. Without this the first sign of a mismatch
    # is "undefined symbol" naming a function that plainly exists in the headers,
    # which reads like a bug in the standard library rather than a stale image.
    if(NOT DEFINED ZHLN_LIBCXX_PROBE_RESULT)
        set(ZHLN_LIBCXX_PROBE_RESULT "")
        set(ZHLN_LIBCXX_PROBE_IMAGE "")
        if(_ZHLN_RUNTIME_DIR)
            foreach(_ZHLN_SUFFIX IN ITEMS dylib so)
                if(EXISTS "${_ZHLN_RUNTIME_DIR}/libc++.${_ZHLN_SUFFIX}")
                    set(ZHLN_LIBCXX_PROBE_IMAGE "${_ZHLN_RUNTIME_DIR}/libc++.${_ZHLN_SUFFIX}")
                    break()
                endif()
            endforeach()
            find_program(ZHLN_NM_EXECUTABLE NAMES llvm-nm nm)
            if(ZHLN_LIBCXX_PROBE_IMAGE AND ZHLN_NM_EXECUTABLE)
                # -gU: external, defined symbols only. Matching the mangled name is
                # exact here, unlike grepping a demangled spelling -- the mangled
                # form contains __hash_memory literally (__ZNSt3__113__hash_memoryEPKvm).
                execute_process(
                    COMMAND "${ZHLN_NM_EXECUTABLE}" -gU "${ZHLN_LIBCXX_PROBE_IMAGE}"
                    OUTPUT_VARIABLE _ZHLN_LIBCXX_SYMS
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(_ZHLN_LIBCXX_SYMS MATCHES "__hash_memory")
                    set(ZHLN_LIBCXX_PROBE_RESULT "ok")
                else()
                    set(ZHLN_LIBCXX_PROBE_RESULT "stale")
                endif()
            endif()
            if(ZHLN_LIBCXX_PROBE_RESULT STREQUAL "ok"
               AND NOT EXISTS "${_ZHLN_RUNTIME_DIR}/libc++abi.dylib"
               AND NOT EXISTS "${_ZHLN_RUNTIME_DIR}/libc++abi.so")
                # A libc++ built from source does not re-export libc++abi the way
                # the platform's umbrella stub does, so -lc++abi has to find a real
                # image in the same directory. When it cannot, only libc++abi's
                # symbols are undefined -- the shape this whole detour started in.
                set(ZHLN_LIBCXX_PROBE_RESULT "no-abi")
            endif()
        endif()
    endif()

    if(ZHLN_LIBCXX_PROBE_RESULT STREQUAL "stale")
        message(FATAL_ERROR
            "libc++ mismatch: ${ZHLN_LIBCXX_PROBE_IMAGE} does not export __hash_memory, which the "
            "libc++ headers in this toolchain declare as living in the dylib "
            "(_LIBCPP_AVAILABILITY_HAS_HASH_MEMORY, LLVM 21). Linking it gives "
            "'undefined symbol: std::__1::__hash_memory' and, for the same reason, "
            "'__from_chars_floating_point' -- both introduced after that image was built.\n"
            "The headers and the dylib have to be the same build: rebuild the runtime "
            "(ninja libcxx libcxxabi in ${LLVM_BLOOMBERG_BUILD}), or point LLVM_BLOOMBERG_ROOT / "
            "LLVM_BLOOMBERG_BUILD at a toolchain whose libc++ is as new as its headers.")
    elseif(ZHLN_LIBCXX_PROBE_RESULT STREQUAL "no-abi")
        message(FATAL_ERROR
            "libc++ is ${ZHLN_LIBCXX_PROBE_IMAGE}, but there is no libc++abi beside it in "
            "${_ZHLN_RUNTIME_DIR}. A libc++ built from source does not re-export libc++abi the way "
            "the platform's umbrella stub does, so -lc++abi needs a real image in that directory; "
            "without one every libc++abi symbol comes out undefined. Build and install libcxxabi "
            "into the same directory, or remove libc++.dylib from it so the whole C++ runtime comes "
            "from the compiler's own search path.")
    elseif(ZHLN_LIBCXX_PROBE_RESULT STREQUAL "ok")
        message(STATUS
            "libc++: linking ${ZHLN_LIBCXX_PROBE_IMAGE} (-L${_ZHLN_RUNTIME_DIR}); "
            "headers and image agree on __hash_memory")
    endif()
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
        target_compile_definitions(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:__cpp_impl_reflection=202603L>")
    endif()
    zahlen_use_custom_libcxx(${TARGET_NAME})
endfunction()
