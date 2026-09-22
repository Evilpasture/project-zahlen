# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Fonts -- the baked font atlas loader (zahlen_fonts).
#
# The consumer of tools/fontbm.sh's output: parse the .fnt (fontbm's JSON
# dialect, through the serialization domain's simdjson reader -- there is no
# second JSON parser in this tree), decode the .png page with stb_image and
# store the result on the UISettingsComponent singleton through the core's
# BakedFontLoader hook. A host that does not build extras never installs the
# hook, and there the engine's runtime TTF parse over system fonts is the
# font.
#
# Public headers are reached as <Fonts/...>.

set(TARGET_NAME zahlen_fonts)

file(GLOB ZHLN_FONTS_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC ${ZHLN_FONTS_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: the loader seeds a core GUI singleton through
# the core's render context and speaks the core's FontAtlas vocabulary.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

# The JSON dialect parses through the serialization domain's opaque reader.
# An implementation detail of this module: JSON.hpp never leaves the parser
# translation unit.
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_serialization
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# The texture page decodes with stb_image; the implementation compiles into
# the engine (extern/stbi_impl.c), so this target needs only the header.
target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/stb
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
