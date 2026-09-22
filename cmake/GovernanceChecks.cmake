# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/GovernanceChecks.cmake
#
# The repository's invariants, enforced at configure time by the Python checks
# in configure/. Every check runs from the repository root and fails the
# configure with the script's own stderr on a violation; zhln_run_governance_check
# is the single place that shape is spelled out, so a check is one call.

# --- PYTHON INTERPRETER SELECTION ---
# Prioritize local virtual environment (.venv) if present
if(EXISTS "${CMAKE_SOURCE_DIR}/.venv")
    set(Python3_ROOT_DIR "${CMAKE_SOURCE_DIR}/.venv")
    set(Python3_FIND_REGISTRY LAST)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(zhln_run_governance_check script_name)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/configure/${script_name}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _zhln_check_result
        OUTPUT_VARIABLE _zhln_check_output
        ERROR_VARIABLE _zhln_check_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _zhln_check_result EQUAL 0)
        message(FATAL_ERROR "${_zhln_check_error}")
    endif()
    if(_zhln_check_output)
        message(STATUS "${_zhln_check_output}")
    endif()
endfunction()

# Enforce the one-way dependency boundary: optional extras may consume core,
# while src/, include/, and modules/ must never consume extras.
zhln_run_governance_check("check_core_extras_boundary.py")

# Tests may only include public headers. src/ is private implementation.
zhln_run_governance_check("check_tests_public_api.py")

# Core implementation directories are hermetic: a subsystem may consume the
# public include/ surface and its own src/<subsystem>/ headers, never a
# sibling's private implementation. Keep this configure-time check in lockstep
# with the target include visibility below.
zhln_run_governance_check("check_subsystem_boundaries.py")

# A PIMPL is encapsulation, not indirection: no first-party class hands its
# implementation out, under any name, to any caller. There is no GetImpl() in the
# tree, and this configure-time check is what keeps one from coming back. The same
# check keeps the presentation seam internal -- no GetPresentationTarget(), and
# PresentationTarget is named by RenderContext's low-level verbs and the window
# subsystem, never by the engine's user-facing API, which hands out render
# attachments (Kernel::AcquireTarget). A class that genuinely needs its contents
# read from elsewhere names a friend instead -- see NativeSurfaceHandle's Visit().
zhln_run_governance_check("check_pimpl_encapsulation.py")

# Reflection internals live in include/Zahlen/Core/Reflection.hpp and the
# modules it includes from include/Zahlen/Core/Reflection/, and nowhere else;
# only Reflection/Core.hpp tests __cpp_impl_reflection/__has_feature(reflection),
# and module interface units must never declare a detail namespace -- module
# internals are internal by not being exported, and a detail namespace only
# exists to be exported by accident. All are configure-time invariants: a stray
# std::meta/^^/[:...:], a second feature test, or a detail namespace would
# otherwise survive as a permanent public surface.
zhln_run_governance_check("check_reflection_boundary.py")

# Macro governance: every #define in the repository must be named in
# configure/macro_allowlist.json. A macro escapes namespaces, types, overloads and
# scope, so it is invisible to every other check here -- which makes an
# allowlist the only place a decision to add one can be recorded.
#
# The deep expansion pass needs the libclang Python bindings and is opt-in: it
# runs as its own target so a machine without them still configures. It is the
# only pass that can tell a macro apart from an enum constant (VK_SUCCESS vs
# VK_FORMAT_R8G8B8A8_UNORM), which a regex cannot do.
zhln_run_governance_check("check_macro_governance.py")

add_custom_target(check_macro_expansions
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/configure/check_macro_governance.py" --full
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking macro expansions with libclang (requires: pip install clang)"
    VERBATIM
)

# Namespace governance: a detail namespace that carries template code is spelled
# TemplatedDetail, and one that carries none has to be named with a reason in
# configure/namespace_allowlist.json. A template is instantiated at its point of use,
# so its definition belongs in a header whether anyone likes it or not -- the
# spelling says so out loud. Everything else had a choice: a translation unit,
# the anonymous namespace in an implementation file, an unexported declaration in
# a module unit. The same pass resolves every qualified Detail::/TemplatedDetail::
# reference against what some namespace of that spelling declares, so a rename
# cannot be left half-finished and a reach into internals cannot outlive the
# internals it reached into.
zhln_run_governance_check("check_namespace_governance.py")

# Module linkage: `inline` is a header word -- it tells the linker that a
# definition may appear in every translation unit that included the header and
# that those copies are one entity. A module unit is compiled once, so in one it
# is noise on a function and worse than noise on a static data member, where no
# symbol is emitted and an importer that inlines a member body is left with an
# undefined reference. There is one spelling that is load-bearing and the check
# insists on it: a namespace-scope constant in an interface unit needs `inline`,
# because const-qualified means internal linkage and an internal-linkage name
# never reaches the interface -- g++ does not diagnose that, it drops the
# declaration and every importer is told the name does not exist.
zhln_run_governance_check("check_module_linkage.py")

# An error is a diagnostic, not a number. ErrorCode keeps its two words private
# and has no conversion to an integral type, so `static_cast<int>` on a code is
# ill-formed by construction -- tests/core/TestError.cpp pins that with
# static_asserts. What the type cannot refuse is the enumerator unpacked out of
# it (`static_cast<uint32_t>(err.As<E>())`) and the words published again
# (`res.error().value`). This check rejects both, and its allowlist names the one
# site that needs the ordinal on purpose: the scripting ABI, which hands the
# script host a number because a foreign category cannot cross as text.
zhln_run_governance_check("check_error_ordinals.py")

# Include provenance: a file must reach every first-party type it spells through
# its own includes, not through whatever its includes happen to include. That
# transitive reliance is what made the old <Zahlen/Types.hpp> load-bearing -- and
# why editing one renderer struct rebuilt the whole engine. The same pass
# resolves every include spelling against the roots a target really has, so a
# quoted include cannot silently pick up an unrelated subsystem's private header
# of the same name, and an include of a header that no longer exists fails here
# rather than at the end of a long build.
zhln_run_governance_check("check_include_provenance.py")
