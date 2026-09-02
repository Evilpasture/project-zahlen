# cmake/CompilerWarnings.cmake

option(ZHLN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CLANG_WARNING_FLAGS
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
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-nested-anon-types
    -Wno-gnu-anonymous-struct

    # Layout & Concurrency
    -Watomic-implicit-seq-cst

    -Rpass-missed=loop-vectorize=.*ZHLN.*
)

set(GCC_WARNING_FLAGS
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
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-nested-anon-types
    -Wno-gnu-anonymous-struct
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wno-interference-size

    # GCC doesn't support path/symbol filtering for -fopt-info-vec-missed,
    # so limit it to optimized builds or opt-in files via pragmas instead of global flags.
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(ZHLN_COMPILE_WARNINGS ${CLANG_WARNING_FLAGS})
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(ZHLN_COMPILE_WARNINGS ${GCC_WARNING_FLAGS})
else()
    set(ZHLN_COMPILE_WARNINGS "")
endif()

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
