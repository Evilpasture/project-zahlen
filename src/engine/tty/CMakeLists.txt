# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Linux TTY / KMS display backend. libseat and libevdev are opened at runtime
# through Zahlen/Core/SharedLibrary.hpp -- this directory does not link them.
# Other platforms compile the inert stubs in TTYBackend.cpp so the engine's
# window path stays uniform.
#
# These sources belong to zahlen_engine: Window and the crash handler call
# TTYBackend::* directly. This directory owns the source list the way
# src/render owns its pass files, rather than folding them into the engine
# directory's RAW_ENGINE_SOURCES.

set(RAW_TTY_SOURCES
    TTYBackend.cpp
)

zahlen_transpile_sources(TTY_SOURCES ${RAW_TTY_SOURCES})
target_sources(zahlen_engine PRIVATE ${TTY_SOURCES})

# Transpiled sources live under the build tree, so quote includes no longer
# resolve beside the original file. Keep this directory on the engine include
# path for both source variants.
target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
