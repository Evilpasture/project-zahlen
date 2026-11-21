# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later


set(RAW_TTY_SOURCES
    TTYBackend.cpp
)

target_sources(zahlen_engine PRIVATE ${RAW_TTY_SOURCES})

target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
