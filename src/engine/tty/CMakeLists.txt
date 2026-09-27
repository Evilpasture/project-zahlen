# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


set(RAW_TTY_SOURCES
    TTYBackend.cpp
)

zahlen_transpile_sources(TTY_SOURCES ${RAW_TTY_SOURCES})
target_sources(zahlen_engine PRIVATE ${TTY_SOURCES})

target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
