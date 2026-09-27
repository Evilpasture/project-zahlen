# cmake/CompilerWarnings.cmake
#
# One candidate set, probed flag by flag: a flag is applied when the probe
# proves the compiler accepts it, so GCC-only diagnostics and Clang-only
# diagnostics coexist in one list and every GNU-style compiler gets exactly
# the subset it understands. No compiler is named anywhere in this file.
#
# Suppressions (-Wno-*) are probed through their POSITIVE spelling: an unknown
# positive is a hard error on every GNU-style driver, while an unknown -Wno-*
# is silently accepted by some of them -- a direct probe of the negative form
# can lie.

option(ZHLN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(CheckCXXCompilerFlag)

set(ZHLN_WARNING_CANDIDATES
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wundef
    -Wvla

    # Layout & Concurrency
    -Watomic-implicit-seq-cst

    # GCC-only diagnostics (Clang drops them in the probe)
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op

    # Clang-only diagnostics (GCC drops them in the probe)
    -Wweak-vtables
    -Rpass-missed=loop-vectorize=.*ZHLN.*
)

# Warnings worth having on when the compiler has them and worth suppressing
# where it does -- listed by their positive spelling (see the header comment).
set(ZHLN_WARNING_SUPPRESSIONS
    -Wunused-parameter
    -Wmissing-field-initializers
    -Wnested-anon-types
    -Wgnu-anonymous-struct
    -Winterference-size
)

set(ZHLN_COMPILE_WARNINGS "")
foreach(_FLAG IN LISTS ZHLN_WARNING_CANDIDATES)
    string(MAKE_C_IDENTIFIER "ZHLN_HAS${_FLAG}" _VAR)
    check_cxx_compiler_flag("${_FLAG}" ${_VAR})
    if(${_VAR})
        list(APPEND ZHLN_COMPILE_WARNINGS "${_FLAG}")
    endif()
endforeach()

foreach(_POS IN LISTS ZHLN_WARNING_SUPPRESSIONS)
    string(MAKE_C_IDENTIFIER "ZHLN_HAS${_POS}" _VAR)
    check_cxx_compiler_flag("${_POS}" ${_VAR})
    if(${_VAR})
        # -Wfoo -> -Wno-foo: suppress only a warning the compiler actually has.
        string(REGEX REPLACE "^-W" "-Wno-" _NEG "${_POS}")
        list(APPEND ZHLN_COMPILE_WARNINGS "${_NEG}")
    endif()
endforeach()

string(JOIN " " ZHLN_COMPILE_WARNINGS_STR ${ZHLN_COMPILE_WARNINGS})

if(NOT TARGET zahlen_warnings)
    add_library(zahlen_warnings INTERFACE)
    target_compile_options(zahlen_warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:${ZHLN_COMPILE_WARNINGS}>
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
    )

    if(ZHLN_WARNINGS_AS_ERRORS)
        target_compile_options(zahlen_warnings INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:-Werror>
            $<$<COMPILE_LANGUAGE:C>:-Werror>
        )
    endif()
endif()
