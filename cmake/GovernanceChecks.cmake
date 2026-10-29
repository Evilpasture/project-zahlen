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
#
# Python is optional (`ZHLN_HAS_PYTHON`): when no Python 3 interpreter is
# available, the Perl provenance check still runs and the Python checks are
# skipped without failing configuration.

include(PythonProbe)
find_program(ZHLN_PERL_EXECUTABLE NAMES perl REQUIRED)

if(ZHLN_HAS_PYTHON)
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
else()
  message(STATUS
    "Python 3 not found (ZHLN_HAS_PYTHON=OFF): skipping Python configure-time governance checks.")
  execute_process(
    COMMAND
      "${ZHLN_PERL_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/configure/check_include_provenance.pl"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _zhln_perl_result
    OUTPUT_VARIABLE _zhln_perl_output
    ERROR_VARIABLE _zhln_perl_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT _zhln_perl_result EQUAL 0)
    message(FATAL_ERROR "${_zhln_perl_error}")
  endif()
  if(_zhln_perl_output)
    message(STATUS "${_zhln_perl_output}")
  endif()
endif()
