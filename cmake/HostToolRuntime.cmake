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
