# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# utils/ztest/CMakeLists.txt
#
# The ztest orchestrator binary. Source wrapping, worker registration and the
# manifest it reads stay in cmake/ZTest.cmake -- this file owns the executable
# only.

add_executable(ztest
    main.cpp
    DebuggerReplay.cpp
)

set_target_properties(ztest PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

zahlen_set_bin_output(ztest)

# Consumes first-party extensions rather than linking the engine: CommandLine and
# Signal are the whole surface it needs (see main.cpp).
target_link_libraries(ztest PRIVATE zahlen_command_line zahlen_signal)

# main.cpp dispatches on reflected enums.
zahlen_enable_reflection(ztest)

if(USE_SANITIZERS)
    zahlen_install_sanitizer_runtime(ztest)
endif()

if(TARGET zahlen_warnings)
    target_link_libraries(ztest PRIVATE zahlen_warnings)
endif()

if(WIN32)
    target_compile_definitions(ztest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
