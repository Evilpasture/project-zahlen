# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/json -- the JSON half of zahlen_serialization.
#
# The target itself is created one directory up, in ../CMakeLists.txt, because
# extras/json and extras/toml are one target: JSONSchema.hpp is reflection-driven
# JSON, TOML.hpp is reflection-driven TOML, and both sit on the same
# Describe/Reflect helpers. What lives here is this directory's translation
# unit and the link to the vendored simdjson the pair reads through.
#
#   JSON.hpp        opaque simdjson document reader
#   JSONSchema.hpp  reflection-driven serialisation plus a compile-time schema
#
# The simdjson itself is vendored at extern/simdjson and provisioned by
# cmake/ThirdParty.cmake; this directory only consumes the simdjson::simdjson
# target it provides.

# simdjson is vendored at extern/simdjson and provisioned by cmake/ThirdParty.cmake
# like every other extern/ entry, so there is no discovery here and none needed:
# a bare Windows toolchain configures this extra with no pkg-config and no
# system package. The guard only catches a forgotten submodule init.
if(NOT TARGET simdjson::simdjson)
    message(FATAL_ERROR
        "simdjson is not provisioned; run 'git submodule update --init extern/simdjson'.")
endif()

file(GLOB ZHLN_JSON_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

if(ZHLN_JSON_SOURCES)
    target_sources(zahlen_serialization PRIVATE ${ZHLN_JSON_SOURCES})
endif()

# simdjson is an implementation detail of JSON.cpp -- JSON.hpp exposes an opaque
# reader and never includes <simdjson.h> -- so the library stays PRIVATE and
# cannot leak into a consumer; the include path travels with the target. Nothing
# in core needs simdjson; this directory is the only reason it is a dependency.
target_link_libraries(zahlen_serialization PRIVATE simdjson::simdjson)
