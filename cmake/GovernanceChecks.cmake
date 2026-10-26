# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/GovernanceChecks.cmake
#
# The repository's invariants, enforced at configure time by the checks in
# configure/:
#   - check_core_layer_boundary.py  (src/, include/, modules/ never consume optional layers)
#   - check_layer_dag.py            (plugins/ and extensions/ never consume gameplay/)
#   - check_tests_public_api.py     (tests/ only consumes public headers)
#   - check_subsystem_boundaries.py (src/<subsystem>/ never reaches sibling internals)
#   - check_pimpl_encapsulation.py  (no PIMPL accessors; PresentationTarget stays internal)
#   - check_reflection_boundary.py  (raw reflection tokens stay in include/Zahlen/Core/Reflection*)
#   - check_macro_governance.py     (every #define allowlisted in configure/macro_allowlist.json)
#   - check_include_governance.py   (unconditional Platform.hpp; Jolt.hpp wrapper in app layers)
#   - check_namespace_governance.py (TemplatedDetail vs allowlisted plain detail; no TU/module detail)
#   - check_module_linkage.py       (inline rules across module interface/implementation units)
#   - check_error_ordinals.py       (no raw ErrorCode ordinal casts outside allowlist)
#   - check_include_provenance.pl   (direct/transitive header provenance across tracked symbols)
#
# configure/run_governance_checks.py runs all 12 checks concurrently across CPU
# cores and caches passing tree fingerprints in ${CMAKE_BINARY_DIR}/.zhln_governance_cache.json
# keyed by (path, mtime_ns, size) of every governed source, CMake file, checker,
# and allowlist (set ZHLN_GOVERNANCE_CACHE=0 to force a full uncached sweep).

# --- CHECK INTERPRETER SELECTION ---
# Resolve the Python 3 interpreter directly via find_program (preferring a
# repository .venv when present) instead of FindPython3, which spawns 8
# sysconfig/SOABI C-extension validation subprocesses on every reconfigure.
if(NOT ZHLN_PYTHON3_EXECUTABLE OR NOT EXISTS "${ZHLN_PYTHON3_EXECUTABLE}")
  if(EXISTS "${CMAKE_SOURCE_DIR}/.venv")
    find_program(
      ZHLN_PYTHON3_EXECUTABLE
      NAMES python3 python
      PATHS "${CMAKE_SOURCE_DIR}/.venv/bin" "${CMAKE_SOURCE_DIR}/.venv/Scripts"
      NO_DEFAULT_PATH)
  endif()
  if(NOT ZHLN_PYTHON3_EXECUTABLE)
    find_program(ZHLN_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)
  endif()
endif()
set(Python3_EXECUTABLE "${ZHLN_PYTHON3_EXECUTABLE}")
if(NOT TARGET Python3::Interpreter)
  add_executable(Python3::Interpreter IMPORTED GLOBAL)
  set_target_properties(Python3::Interpreter PROPERTIES
    IMPORTED_LOCATION "${Python3_EXECUTABLE}")
endif()
find_program(ZHLN_PERL_EXECUTABLE NAMES perl REQUIRED)

execute_process(
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configure/run_governance_checks.py"
    --perl "${ZHLN_PERL_EXECUTABLE}"
    --cache-file "${CMAKE_BINARY_DIR}/.zhln_governance_cache.json"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE _zhln_check_result
  OUTPUT_VARIABLE _zhln_check_output
  ERROR_VARIABLE _zhln_check_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)

if(NOT _zhln_check_result EQUAL 0)
  message(FATAL_ERROR "${_zhln_check_error}")
endif()

if(_zhln_check_output)
  string(REPLACE ";" "\\;" _zhln_check_lines "${_zhln_check_output}")
  string(REPLACE "\n" ";" _zhln_check_lines "${_zhln_check_lines}")
  foreach(_zhln_line IN LISTS _zhln_check_lines)
    if(_zhln_line)
      message(STATUS "${_zhln_line}")
    endif()
  endforeach()
endif()

# The deep macro-expansion pass needs the libclang Python bindings and is
# opt-in: it runs as its own target so a machine without them still configures.
add_custom_target(
  check_macro_expansions
  COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_SOURCE_DIR}/configure/check_macro_governance.py" --full
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT
    "Checking macro expansions with libclang (requires: pip install clang)"
  VERBATIM)
