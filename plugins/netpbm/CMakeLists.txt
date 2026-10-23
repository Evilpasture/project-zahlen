# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# plugins/netpbm -- Netpbm serialization of a captured frame (zahlen_netpbm).
#
#   PpmEncoder.hpp  ZHLN::Netpbm::EncodePPM -- a CapturedFrame to the bytes of
#                   a P6 (PPM) or P7 (PAM) file, in memory, no I/O.
#
# Header-only on purpose: encoding is arithmetic over a byte span, so there is
# nothing to compile and nothing to link. The target exists so a consumer that
# wants a .ppm names that dependency instead of borrowing another plugin's
# include root, and so the plugin appears in the graph like every other one.
#
# Why this is not core: a file format is a choice a caller makes. Core hands out
# pixels (include/Zahlen/Render/CapturedFrame.hpp) and knows nothing about P6
# headers, exactly as it knows nothing about glTF or JSON. The fidelity harness
# and the GPU test helpers are the layers that own a path, and this is what they
# encode with.
#
# Public headers are reached as <netpbm/PpmEncoder.hpp>.

set(TARGET_NAME zahlen_netpbm)

add_library(${TARGET_NAME} INTERFACE)

# CapturedFrame is a core type; the target carries the core include root the
# same way a compiling plugin links zahlen_engine.
target_link_libraries(${TARGET_NAME} INTERFACE zahlen_engine)

target_include_directories(${TARGET_NAME} INTERFACE
    ${PROJECT_SOURCE_DIR}/plugins
)
