# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/HostToolRuntime.cmake
#
# Where a tool the build runs finds its own C++ runtime.
#
# The build runs code before it compiles the engine: zshader reflects the cooked
# shaders into the catalog (and, in the offline flow, zcook cooks them), and the
# catalog is what src/render compiles against. Those tools are linked with the
# target toolchain like everything else, so under ZHLN_USE_CUSTOM_LIBCXX they
# want the libc++/libc++abi/libunwind that live inside a custom LLVM prefix --
# which is on no loader path, and in an LLVM build tree usually one directory
# below the lib directory, under a target triple. The first tool the build
# tries to run then fails before it can say anything else:
#
#   build/p2996/zshader: error while loading shared libraries:
#   libunwind.so.1: cannot open shared object file: No such file or directory
#
# LIBRARY targets in this project get those paths from
# zahlen_use_custom_libcxx(); they are linked, not run, so it never showed up
# there. This file is the same idea for every target created after it is
# included: CMAKE_BUILD_RPATH is the initial BUILD_RPATH of each of them, so the
# prefix's runtime directories reach the libraries and the executables in one
# place instead of one call site per tool.
#
# It is inert unless ZHLN_USE_CUSTOM_LIBCXX is ON and the compiler actually
# speaks -stdlib=libc++ (feature-probed at the root) -- the GCC and
# system-Clang builds have their runtime on the loader's path already, and
# CMAKE_BUILD_RPATH stays unset.
#
# Set ZHLN_BUILD_RUNTIME_DIRS to add directories by hand, for a prefix this file
# cannot guess (semicolon-separated, like any CMake list).

if(ZHLN_USE_CUSTOM_LIBCXX AND ZHLN_HAS_STDLIB_LIBCXX)
    set(_zhln_runtime_dirs "")

    # The prefix as tools/build.sh passes it (LLVM_BLOOMBERG_BUILD points at the
    # build tree, which is where an LLVM build puts the runtimes it has just
    # built), plus the two conventional layouts beside it.
    foreach(_zhln_prefix IN ITEMS "${LLVM_BLOOMBERG_BUILD}" "${LLVM_BLOOMBERG_ROOT}/install" "${LLVM_BLOOMBERG_ROOT}/build")
        if(IS_DIRECTORY "${_zhln_prefix}/lib" OR IS_DIRECTORY "${_zhln_prefix}/lib64")
            # lib, lib64, and one level down: lib/<target-triple>/.
            file(GLOB _zhln_candidates LIST_DIRECTORIES true
                "${_zhln_prefix}/lib" "${_zhln_prefix}/lib64"
                "${_zhln_prefix}/lib/*" "${_zhln_prefix}/lib64/*"
            )
            foreach(_zhln_candidate IN LISTS _zhln_candidates)
                if(IS_DIRECTORY "${_zhln_candidate}")
                    list(APPEND _zhln_runtime_dirs "${_zhln_candidate}")
                endif()
            endforeach()
        endif()
    endforeach()

    # Whatever the compiler itself reports: the resource and runtime directories
    # of a custom toolchain are in here even when the prefix is somewhere else.
    foreach(_zhln_dir IN LISTS CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES)
        list(APPEND _zhln_runtime_dirs "${_zhln_dir}")
    endforeach()

    foreach(_zhln_dir IN LISTS ZHLN_BUILD_RUNTIME_DIRS)
        list(APPEND _zhln_runtime_dirs "${_zhln_dir}")
    endforeach()

    if(_zhln_runtime_dirs)
        list(REMOVE_DUPLICATES _zhln_runtime_dirs)
        set(CMAKE_BUILD_RPATH "${_zhln_runtime_dirs}")
        message(STATUS "Build-time runtime paths (rpath for every target): ${_zhln_runtime_dirs}")
    else()
        message(STATUS "Build-time runtime paths: none found -- set ZHLN_BUILD_RUNTIME_DIRS if a tool cannot start")
    endif()
endif()

# --- Windows: the same question, asked again at load time ---
#
# On ELF a shared library's location is a property of the file: the rpath CMake
# writes into every target in the build tree names the directories the tool's
# libraries are in, and the loader reads it before the tool runs an instruction.
# PE has no such field. There the loader searches the directory the image was
# loaded from, then the system directories, then PATH -- and the build tree is on
# none of them, so the first tool the build tries to run dies before main(), with
# nothing on stdout to explain it:
#
#   build/shared_assets/zcook.exe
#   Exit code 0xc0000135 (3221225781) -- STATUS_DLL_NOT_FOUND
#
# 0xc0000135 is the loader saying it could not find a DLL the image imports. For
# zcook that is libzahlen_engine.dll (in build/, beside the zcook a project that
# shares the cooked-asset cache reaches through the stable symlink), libJolt.dll
# (build/extern/JoltPhysics/Build/, a directory nothing searches), and the
# toolchain's own runtime -- libstdc++-6.dll, libgcc_s_seh-1.dll,
# libwinpthread-1.dll and libzstd.dll -- which MSYS2 keeps in the compiler's own
# directory, ucrt64/bin: a directory an MSYS2 shell has on PATH and a build
# started from cmd.exe does not.
#
# The directories go on PATH for the duration of the command instead of being
# copied beside the tool. A copy is a snapshot that goes stale the first time Jolt
# is relinked while zcook is not, and the loader would then quietly pick the old
# library up; this names the directories the artifacts are in, so it cannot drift.
#
# ZHLN_TOOL_ENV_COMMAND and ZHLN_TOOL_ENV_ARGUMENT are the words a module prepends
# to a command that runs a tool out of this build:
#
#   COMMAND ${ZHLN_TOOL_ENV_COMMAND} "${ZHLN_TOOL_ENV_ARGUMENT}" <tool> ...
#
# Both are empty where this does not apply -- every non-Windows build, where the
# rpath is the answer -- so those command lines stay byte-for-byte what they were.
# The PATH argument is spelled the way ShaderCompilation.cmake already spells it
# for zshader: the separators escaped, so CMake hands cmake -E env a single
# argument, and $ENV{PATH} kept on the end, so the tools that command then runs --
# ninja, python, git -- are still found. zahlen_add_runtime_dll_path() is the same
# directories for a test, whose binary is built into build/tests/, not beside the
# DLLs it links.
#
# Set ZHLN_TOOL_RUNTIME_DIRS to add directories by hand, for a prefix this file
# cannot guess (semicolon-separated, like any CMake list).

set(ZHLN_RUNTIME_DLL_DIRS "")
set(ZHLN_RUNTIME_DLL_PATH "")
set(ZHLN_TOOL_ENV_COMMAND "")
set(ZHLN_TOOL_ENV_ARGUMENT "")

if(WIN32)
    # The project's own shared libraries: the engine's, and Jolt's, which is built
    # in a subdirectory of the build tree and so has no loader path either.
    list(APPEND ZHLN_RUNTIME_DLL_DIRS
        "$<TARGET_FILE_DIR:zahlen_engine>"
        "$<TARGET_FILE_DIR:Jolt>"
    )

    # The toolchain's runtime, from the directory the compiler itself is in -- the
    # way tests/CMakeLists.txt finds the ASan runtime DLL beside clang.
    get_filename_component(_zhln_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND ZHLN_RUNTIME_DLL_DIRS "${_zhln_toolchain_bin}")

    foreach(_zhln_dir IN LISTS ZHLN_TOOL_RUNTIME_DIRS)
        list(APPEND ZHLN_RUNTIME_DLL_DIRS "${_zhln_dir}")
    endforeach()

    list(REMOVE_DUPLICATES ZHLN_RUNTIME_DLL_DIRS)

    # One string, separators escaped: see the note above. The escape is generated
    # here rather than written at each use because it is the escape that keeps the
    # value a single argument, and a value passed through a list variable does not
    # keep it.
    string(REPLACE ";" "\\;" ZHLN_RUNTIME_DLL_PATH "${ZHLN_RUNTIME_DLL_DIRS}")
    set(ZHLN_TOOL_ENV_COMMAND ${CMAKE_COMMAND} -E env)
    set(ZHLN_TOOL_ENV_ARGUMENT "PATH=${ZHLN_RUNTIME_DLL_PATH}\;$ENV{PATH}")

    # The write-out names the generator expressions as they are written above:
    # they resolve per target, at generate time.
    message(STATUS "Host tool DLL directories (PATH for the tools the build runs): ${ZHLN_RUNTIME_DLL_DIRS}")
endif()

# The same directories, for one registered test. A test binary is built into
# build/tests/... -- not beside the DLLs it links -- so it needs the loader path
# whether or not it runs a tool of its own, and the ones that run zcook pass it on
# to a grandchild process.
#
# APPEND, because the groups own ENVIRONMENT for their own variables (the
# sanitizer profile, the Vulkan sandbox pin) and this must not clobber them; and
# path_list_prepend rather than an ENVIRONMENT entry, which would replace PATH
# outright and drop whatever PATH the test was started with.
function(zahlen_add_runtime_dll_path TEST_NAME)
    if(ZHLN_RUNTIME_DLL_DIRS)
        set_property(TEST "${TEST_NAME}" APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${ZHLN_RUNTIME_DLL_PATH}"
        )
    endif()
endfunction()

# The same directories again, this time copied beside one built binary.
#
# The two halves of the problem look alike and are not. Rounds 4 and 5 are about
# commands the build runs itself, and there the directories go on PATH for the
# duration of the command -- a copy would be a snapshot that goes stale the first
# time a DLL is relinked. This is the other half: a binary a person runs. On ELF
# the build tree's rpath answers that -- CMake writes the directory of every
# library a target links into the executable, so build/<tag>/zahlen starts in the
# build tree. PE has no such field: the loader searches the image's own directory,
# then the system directories, then PATH, and the build tree is on none of them:
#
#   build/current/zahlen.exe: error while loading shared libraries:
#   libzahlen_engine.dll: cannot open shared object file: No such file or directory
#
# The engine's DLL does land beside the executable -- its target lives in the root
# CMakeLists.txt, so the DLL goes to the root of the build tree -- but Jolt's is
# built in a subdirectory of the build tree, and the loader does not search there,
# so the engine's own dependency cannot be resolved. $<TARGET_RUNTIME_DLLS:...> is
# every DLL of the target's dependencies that CMake knows about, Jolt's included
# through the engine, and they are copied next to the binary: a build tree binary
# then runs from any shell, without a PATH its caller has to be told about.
#
# cmake -E copy_if_different, and refreshed by the build itself: CMake keeps these
# DLLs as dependencies of the command, so relinking one re-runs the copy. See
# evidence/repro10_runtime_dlls.txt for the measurement, including that part.
#
# Called for executables that link a shared library -- the list is never empty
# there, which matters because an empty list would leave `cmake -E
# copy_if_different` with a destination and nothing to copy.
function(zahlen_add_runtime_dlls TARGET_NAME)
    if(NOT WIN32)
        return()
    endif()
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${TARGET_NAME}>
                $<TARGET_FILE_DIR:${TARGET_NAME}>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying the runtime DLLs beside ${TARGET_NAME}"
    )
endfunction()
