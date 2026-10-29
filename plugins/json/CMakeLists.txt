# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# plugins/json -- the JSON half of zahlen_serialization.
#
# The target itself is created one directory up, in ../CMakeLists.txt, because
# plugins/json and plugins/toml are one target: JSONSchema.hpp is reflection-driven
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

# simdjson is provisioned by cmake/ThirdParty.cmake (system package when
# ZHLN_HAS_SIMDJSON is true, otherwise vendored extern/simdjson); this directory
# only consumes the simdjson::simdjson target it provides.
if(NOT TARGET simdjson::simdjson)
    message(FATAL_ERROR
        "simdjson is not provisioned; install system simdjson or run 'git submodule update --init extern/simdjson'.")
endif()

set(ZHLN_JSON_SOURCES
    JSON.cpp
)

target_sources(zahlen_serialization PRIVATE ${ZHLN_JSON_SOURCES})

# simdjson is an implementation detail of JSON.cpp -- JSON.hpp exposes an opaque
# reader and never includes <simdjson.h> -- so the library stays PRIVATE and
# cannot leak into a consumer; the include path travels with the target. Nothing
# in core needs simdjson; this directory is the only reason it is a dependency.
target_link_libraries(zahlen_serialization PRIVATE simdjson::simdjson)
