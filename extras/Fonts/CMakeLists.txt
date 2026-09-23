# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Fonts -- the production baked-font path (zahlen_fonts).
#
# The engine consumes pre-baked glyph atlases only (core never parses an
# outline font: that lives in `zcook font`). This extra feeds those atlases
# from the formats shops bake offline:
#
#   * fontbm `.fnt` + `.png` pairs -- the AngelCode BMFont text descriptor and
#     its coverage page;
#   * cooked 'FNT0' containers from a mounted pak or from disk.
#
# InstallBakedFontLoader (Fonts.hpp) installs core's BakedFontLoader hook; the
# composition root calls it before InitializeDefaultScene and core takes over.
# stb_image is not compiled here -- STB_IMAGE_IMPLEMENTATION lives in
# extern/stbi_impl.c inside zahlen_engine, so the decode symbols arrive through
# the engine link, the same way extras/glTF gets them.
#
# Public headers are reached as <Fonts/Fonts.hpp>.

set(TARGET_NAME zahlen_fonts)

add_library(${TARGET_NAME} STATIC
    Fonts.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/stb
)

# Core is a public dependency: Fonts.hpp names Engine and the GUI baked-font
# seam in its signatures.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_serialization
    zahlen_warnings
)

# CreativeWorksManager.hpp and friends drag in the C++26 reflection headers;
# every target that compiles against them needs the -freflection flag.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()
