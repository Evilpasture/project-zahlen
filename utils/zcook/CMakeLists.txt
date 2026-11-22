# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# utils/zcook/CMakeLists.txt
#
# ONLY the optional zcook CLI: argument parsing, file IO, the Blender ZMET
# intermediate bridge (IR.hpp/BinaryReader.hpp), the GLB emitter, and the
# Ninja graph generator. All reusable cooking/baking logic lives in
# plugins/AssetCooking (Zahlen::AssetCooking) -- this directory holds no
# library and ships no headers. Configured from the root CMakeLists as an
# extras tool (next to utils/scene), never from AssetPipeline.

add_executable(zcook
    main.cpp
    Cook.cpp
    FontBake.cpp
    GLB.cpp
    Ninja.cpp
)
# zahlen_serialization is linked for GLB.cpp's <json/JSONSchema.hpp>;
# the AssetCooking library deliberately stays free of it.
target_link_libraries(zcook PRIVATE
    zahlen_engine
    zahlen_image
    zahlen_asset_cooking
    zahlen_serialization
    zahlen_warnings
)
target_include_directories(zcook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
zahlen_enable_reflection(zcook)
zahlen_set_bin_output(zcook)
zahlen_add_runtime_dlls(zcook)
