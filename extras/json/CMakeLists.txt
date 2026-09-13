# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/json -- the JSON half of zahlen_serialization.
#
# The target itself is created one directory up, in ../CMakeLists.txt, because
# extras/json and extras/toml are one target: JSONSchema.hpp is reflection-driven
# JSON, TOML.hpp is reflection-driven TOML, and both sit on the same
# Describe/Reflect helpers. What lives here is this directory's translation unit
# and the discovery of the one third-party dependency the pair owns.
#
#   JSON.hpp        opaque simdjson document reader
#   JSONSchema.hpp  reflection-driven serialisation plus a compile-time schema

find_package(PkgConfig REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)

    # Ensure alias exists for consistent linking
    if(TARGET PkgConfig::SIMDJSON AND NOT TARGET simdjson::simdjson)
        add_library(simdjson::simdjson ALIAS PkgConfig::SIMDJSON)
    endif()
else()
    message(WARNING "extras/json needs simdjson; no package-config discovery is set up for this platform.")
endif()

file(GLOB ZHLN_JSON_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

if(ZHLN_JSON_SOURCES)
    target_sources(zahlen_serialization PRIVATE ${ZHLN_JSON_SOURCES})
endif()

# simdjson is an implementation detail of JSON.cpp -- JSON.hpp exposes an opaque
# reader and never includes <simdjson.h> -- so the include path and the library
# both stay PRIVATE and cannot leak into a consumer. Nothing in core needs
# simdjson; this directory is the only reason it is a dependency at all.
if(SIMDJSON_INCLUDE_DIRS)
    target_include_directories(zahlen_serialization PRIVATE ${SIMDJSON_INCLUDE_DIRS})
endif()
if(TARGET PkgConfig::SIMDJSON)
    target_link_libraries(zahlen_serialization PRIVATE PkgConfig::SIMDJSON)
endif()
