# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/PythonProbe.cmake
#
# Optional Python 3 feature detection (`ZHLN_HAS_PYTHON`).
#
# Resolves a working Python 3.10+ interpreter directly via find_program
# (preferring a repository .venv when present) instead of FindPython3, which
# spawns 8 sysconfig/SOABI C-extension validation subprocesses on every
# reconfigure. Validates the discovered binary once so Windows Store execution
# aliases (`WindowsApps\python3.exe`), broken venv symlinks, and Python 2 do
# not masquerade as an installed interpreter.
#
# Python is optional when compiling with a native `-freflection` toolchain:
#   - ZHLN_HAS_PYTHON=TRUE  -> defines Python3_EXECUTABLE and Python3::Interpreter
#   - ZHLN_HAS_PYTHON=FALSE -> skips Python configure checks; build proceeds
#                              using native C++ host tools (zshader, zcook)
# Pass `-DZHLN_HAS_PYTHON=OFF` to force the Python-free build path on a host
# that has Python installed, or `-DZHLN_HAS_PYTHON=ON` to require Python.

include_guard(GLOBAL)

set(_zhln_python_forced_off FALSE)
set(_zhln_python_forced_on FALSE)
if(DEFINED ZHLN_HAS_PYTHON)
    if(ZHLN_HAS_PYTHON)
        set(_zhln_python_forced_on TRUE)
    else()
        set(_zhln_python_forced_off TRUE)
    endif()
endif()

if(_zhln_python_forced_off)
    set(ZHLN_HAS_PYTHON FALSE)
    set(Python3_EXECUTABLE "")
else()
    if(NOT ZHLN_PYTHON3_EXECUTABLE OR NOT EXISTS "${ZHLN_PYTHON3_EXECUTABLE}")
        unset(ZHLN_PYTHON3_EXECUTABLE CACHE)
        unset(_ZHLN_VERIFIED_PYTHON3 CACHE)
        if(EXISTS "${CMAKE_SOURCE_DIR}/.venv")
            find_program(
                ZHLN_PYTHON3_EXECUTABLE
                NAMES python3 python
                PATHS "${CMAKE_SOURCE_DIR}/.venv/bin" "${CMAKE_SOURCE_DIR}/.venv/Scripts"
                NO_DEFAULT_PATH)
        endif()
        if(NOT ZHLN_PYTHON3_EXECUTABLE)
            find_program(ZHLN_PYTHON3_EXECUTABLE NAMES python3 python)
        endif()
    endif()

    if(ZHLN_PYTHON3_EXECUTABLE AND NOT "${_ZHLN_VERIFIED_PYTHON3}" STREQUAL "${ZHLN_PYTHON3_EXECUTABLE}")
        execute_process(
            COMMAND "${ZHLN_PYTHON3_EXECUTABLE}" -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
            RESULT_VARIABLE _zhln_py_probe_rc
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(_zhln_py_probe_rc EQUAL 0)
            set(_ZHLN_VERIFIED_PYTHON3 "${ZHLN_PYTHON3_EXECUTABLE}" CACHE INTERNAL "Verified Python 3.10+ interpreter path")
        else()
            unset(ZHLN_PYTHON3_EXECUTABLE CACHE)
            unset(_ZHLN_VERIFIED_PYTHON3 CACHE)
        endif()
    endif()

    if(ZHLN_PYTHON3_EXECUTABLE AND "${_ZHLN_VERIFIED_PYTHON3}" STREQUAL "${ZHLN_PYTHON3_EXECUTABLE}")
        set(ZHLN_HAS_PYTHON TRUE)
        set(Python3_EXECUTABLE "${ZHLN_PYTHON3_EXECUTABLE}")
        if(NOT TARGET Python3::Interpreter)
            add_executable(Python3::Interpreter IMPORTED GLOBAL)
            set_target_properties(Python3::Interpreter PROPERTIES
                IMPORTED_LOCATION "${Python3_EXECUTABLE}")
        endif()
    else()
        unset(ZHLN_PYTHON3_EXECUTABLE CACHE)
        unset(_ZHLN_VERIFIED_PYTHON3 CACHE)
        if(_zhln_python_forced_on)
            message(FATAL_ERROR
                "ZHLN_HAS_PYTHON=ON was requested, but no working Python 3.10+ interpreter was found on PATH or in .venv.")
        endif()
        set(ZHLN_HAS_PYTHON FALSE)
        set(Python3_EXECUTABLE "")
    endif()
endif()
