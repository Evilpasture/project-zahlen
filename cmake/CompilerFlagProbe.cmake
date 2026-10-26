# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/CompilerFlagProbe.cmake
#
# Fast C++ compiler flag probing without spawning one full CMake+Ninja+linker
# try_compile project per flag.
#
# Compiles a minimal C++26 translation unit to an object file (-c) under -Werror
# and checks both exit status and driver diagnostics. Batch probing uses
# binary-split recursion: when a slice of flags is accepted together, one
# compiler invocation validates the entire slice; when any flag in a slice is
# rejected, the slice splits until unsupported flags are isolated at leaf size 1.

include_guard(GLOBAL)

function(_zhln_prepare_cxx_flag_probe OUT_CMD_LIST OUT_SRC OUT_OBJ)
    set(_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles")
    file(MAKE_DIRECTORY "${_probe_dir}")
    set(_src "${_probe_dir}/zhln_flag_probe.cpp")
    set(_obj "${_probe_dir}/zhln_flag_probe.o")
    if(NOT EXISTS "${_src}")
        file(WRITE "${_src}" "int main() { return 0; }\n")
    endif()

    set(_cmd "${CMAKE_CXX_COMPILER}")
    if(CMAKE_CXX_COMPILER_ARG1)
        separate_arguments(_arg1 NATIVE_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
        list(APPEND _cmd ${_arg1})
    endif()
    if(CMAKE_CXX_FLAGS)
        separate_arguments(_cxxflags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
        list(APPEND _cmd ${_cxxflags})
    endif()
    if(CMAKE_CXX26_STANDARD_COMPILE_OPTION)
        list(APPEND _cmd ${CMAKE_CXX26_STANDARD_COMPILE_OPTION})
    else()
        list(APPEND _cmd -std=c++26)
    endif()
    if(CMAKE_OSX_SYSROOT)
        list(APPEND _cmd -isysroot "${CMAKE_OSX_SYSROOT}")
    endif()

    set(${OUT_CMD_LIST} "${_cmd}" PARENT_SCOPE)
    set(${OUT_SRC} "${_src}" PARENT_SCOPE)
    set(${OUT_OBJ} "${_obj}" PARENT_SCOPE)
endfunction()

function(_zhln_run_cxx_flag_compile CMD_LIST SRC OBJ FLAG_WORDS OUT_OK)
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(
            COMMAND ${CMD_LIST} /nologo /WX ${FLAG_WORDS} /c "${SRC}" "/Fo${OBJ}"
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err
        )
    else()
        execute_process(
            COMMAND ${CMD_LIST} -Werror ${FLAG_WORDS} -c "${SRC}" -o "${OBJ}"
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err
        )
    endif()
    set(_ok FALSE)
    if(_res EQUAL 0)
        string(TOLOWER "${_out}\n${_err}" _diag)
        if(NOT _diag MATCHES "(unrecognized|unknown|unsupported|ignoring unknown)[^a-z0-9]*option|unknown argument|warning: .*ignored|d9002")
            set(_ok TRUE)
        endif()
    endif()
    set(${OUT_OK} ${_ok} PARENT_SCOPE)
endfunction()

function(zhln_check_cxx_compiler_flag FLAG_STR OUT_VAR)
    if(DEFINED ${OUT_VAR})
        return()
    endif()
    _zhln_prepare_cxx_flag_probe(_cmd _src _obj)
    separate_arguments(_flag_words NATIVE_COMMAND "${FLAG_STR}")
    _zhln_run_cxx_flag_compile("${_cmd}" "${_src}" "${_obj}" "${_flag_words}" _ok)
    if(_ok)
        set(${OUT_VAR} 1 CACHE INTERNAL "Test ${OUT_VAR}")
    else()
        set(${OUT_VAR} "" CACHE INTERNAL "Test ${OUT_VAR}")
    endif()
endfunction()

function(_zhln_probe_flag_slice CMD_LIST SRC OBJ FLAGS_SLICE VARS_SLICE)
    list(LENGTH FLAGS_SLICE _len)
    if(_len EQUAL 0)
        return()
    endif()

    _zhln_run_cxx_flag_compile("${CMD_LIST}" "${SRC}" "${OBJ}" "${FLAGS_SLICE}" _batch_ok)
    if(_batch_ok)
        math(EXPR _last "${_len} - 1")
        foreach(_idx RANGE 0 ${_last})
            list(GET VARS_SLICE ${_idx} _v)
            set(${_v} 1 CACHE INTERNAL "Test ${_v}")
        endforeach()
        return()
    endif()

    if(_len EQUAL 1)
        list(GET VARS_SLICE 0 _v)
        set(${_v} "" CACHE INTERNAL "Test ${_v}")
        return()
    endif()

    math(EXPR _mid "${_len} / 2")
    math(EXPR _rem "${_len} - ${_mid}")
    list(SUBLIST FLAGS_SLICE 0 ${_mid} _left_flags)
    list(SUBLIST VARS_SLICE 0 ${_mid} _left_vars)
    list(SUBLIST FLAGS_SLICE ${_mid} ${_rem} _right_flags)
    list(SUBLIST VARS_SLICE ${_mid} ${_rem} _right_vars)

    _zhln_probe_flag_slice("${CMD_LIST}" "${SRC}" "${OBJ}" "${_left_flags}" "${_left_vars}")
    _zhln_probe_flag_slice("${CMD_LIST}" "${SRC}" "${OBJ}" "${_right_flags}" "${_right_vars}")
endfunction()

function(zhln_check_cxx_compiler_flags_batch FLAGS_LIST)
    set(_uncached_flags "")
    set(_uncached_vars "")
    foreach(_flag IN LISTS FLAGS_LIST)
        string(MAKE_C_IDENTIFIER "ZHLN_HAS${_flag}" _var)
        if(NOT DEFINED ${_var})
            list(APPEND _uncached_flags "${_flag}")
            list(APPEND _uncached_vars "${_var}")
        endif()
    endforeach()

    list(LENGTH _uncached_flags _num_uncached)
    if(_num_uncached EQUAL 0)
        return()
    endif()

    _zhln_prepare_cxx_flag_probe(_cmd _src _obj)
    _zhln_probe_flag_slice("${_cmd}" "${_src}" "${_obj}" "${_uncached_flags}" "${_uncached_vars}")
endfunction()
