# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Header-only internal presentation seam shared by producers (zahlen_window)
# and consumers (zahlen_render) with zero compiled objects or windowing deps.
add_library(zahlen_presentation INTERFACE)
# Rooted like every other first-party header: consumers spell
# "presentation/PresentationTarget.hpp". Publishing this directory instead is
# what put a second root on zahlen_render's command line, so a bare
# "PresentationTarget.hpp" could resolve for render TUs but nothing else.
target_include_directories(zahlen_presentation INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
