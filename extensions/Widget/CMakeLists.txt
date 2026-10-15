# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Widget -- the widgets a panel is built from (zahlen_widgets).
#
# The rolling window a history is kept in, the bar strip a history is drawn as,
# the card an element opens on hover, and the text measurement that keeps a line
# inside its box. Each is a pattern that panels were carrying their own copy of,
# and each copy was a place for the same bug: a chart that indexed its ring from
# the wrong end and froze; a sparkline whose zero-cost rows filled every bar; a
# fitted line that still wrapped. The pattern lives here once, with the invariant
# written down and a suite behind it.
#
# Core is the only dependency, and it is public: a widget draws through
# GUI::Context, which is core's. Nothing here knows about a platform, a device,
# a telemetry source or a benchmark -- the point of the layer is that a panel
# hands it samples and rectangles, and a test can do the same headlessly.
#
# Public headers are reached as <Widget/...>.

set(TARGET_NAME zahlen_widgets)

set(ZHLN_WIDGET_SOURCES
    BarStrip.cpp
    HoverCard.cpp
    TextFit.cpp
)

add_library(${TARGET_NAME} STATIC ${ZHLN_WIDGET_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core, and only core: the widgets are GUI::Context calls plus arithmetic. They
# read no component and reflect no type, and can be linked by a test with no
# engine running. The house call below is not about this target's own code: it
# is what puts a target on the same option set as the engine (see
# cmake/ReflectionConfig.cmake -- a consumer whose options differ makes CMake
# build a second set of the engine's BMIs), and it is the only route to
# zahlen_use_custom_libcxx(), which every other target in this tree takes.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
