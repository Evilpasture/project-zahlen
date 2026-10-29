# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Header-only internal presentation seam shared by producers (zahlen_window)
# and consumers (zahlen_render) with zero compiled objects or windowing deps.
add_library(zahlen_presentation INTERFACE)
target_include_directories(zahlen_presentation INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
