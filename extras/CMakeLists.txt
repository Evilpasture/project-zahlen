# extras/CMakeLists.txt
option(ZHLN_BUILD_EXTRAS "Build first-party engine feature modules and VFX toolkits" ON)

if(NOT ZHLN_BUILD_EXTRAS)
    return()
endif()

# The dependency boundary is one-way: everything in this directory may consume
# the core engine, and nothing in src/, include/ or modules/ may consume
# anything here (tools/check_core_extras_boundary.py enforces it at configure
# time). That is what lets the third-party dependencies below stay optional --
# simdjson in particular is needed by extras/json and by nothing in core.
#
# extras/ is not one library. It is a set of domains with almost nothing in
# common: a network serializer that wants no renderer, a glTF importer that owns
# three vendored libraries, an artificial-life simulator that wants no graphics
# at all, and a JSON reader that is the only reason simdjson is a dependency.
# Each domain therefore has its own target and its own CMakeLists.txt, which owns
# both the sources and the dependencies of that domain:
#
#   Animation/  zahlen_animation     ZHLN.Rig / ZHLN.Animator / ZHLN.Locomotion /
#                                    ZHLN.ProceduralAnimation and the units behind
#                                    them. Rig maths over Jolt vectors and the ECS.
#   Network/    zahlen_network       ZHLN.Wire + ZHLN.Network. Isolates the wire
#                                    format from the renderer and from simdjson.
#   ALife/      zahlen_alife         factions, GOAP, graph, spatial grid,
#                                    simulator. No graphics dependencies.
#   VFX/        zahlen_vfx           ZHLN.CombatFX / ZHLN.Explosions /
#                                    ZHLN.Lightning.
#   glTF/       zahlen_gltf          the importer; owns cgltf, meshoptimizer, stb.
#   json/toml/  zahlen_serialization reflection-driven documents; owns simdjson.
#
# Five more directories were already targets of their own and stay that way, each
# for the reason given in its own file:
#
#   Scripting/  zahlen_scripting + zahlen_scripting_lua (LuaJIT is the only
#               runtime dependency in the tree, and a consumer of the extras must
#               not be forced to link a Lua runtime it does not want)
#   editor/     zahlen_editor (the composition root gates --editor on
#               ZHLN_HAS_EDITOR rather than giving every consumer the editor)
#   Console/    zahlen_console (depends on zahlen_scripting)
#   SVG/        zahlen_svg (resvg is optional; no target at all when absent)
#   HTTP/       zahlen_http (libcurl is optional; no target at all when absent)
#
# Because every domain lists its own sources, there are no recursive source globs
# here any more. A file added to one of these directories is compiled by that
# directory's target, by exactly one target, and a new domain is a new
# CMakeLists.txt plus one add_subdirectory below.
#
# zahlen_extras, at the bottom of this file, is the aggregate: an INTERFACE
# target that links the six domains above and nothing else. It exists for
# consumers that want all of extras the way they used to get it; consumers that
# want one domain link that domain.

# --- serialization -----------------------------------------------------------
# One target, two directories: extras/json (reflection-driven JSON, owns
# simdjson) and extras/toml (reflection-driven TOML, no dependencies). The target
# is created here rather than in either subdirectory so neither one has to know
# about the other, and so the properties that belong to the pair -- the engine
# link, the published include root, the reflection flags -- are set in one place.
add_library(zahlen_serialization STATIC)

add_subdirectory(json)
add_subdirectory(toml)

set_target_properties(zahlen_serialization PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: JSON.hpp and TOML.hpp both include
# <Zahlen/Error.hpp> and return std::expected<T, ZHLN::Error>, and SceneTOML.hpp
# binds Jolt vectors, so a consumer needs core's include path and its
# error-category registration, not just this archive.
target_link_libraries(zahlen_serialization PUBLIC
    zahlen_engine
)

# Public headers are included as <json/JSON.hpp> and <toml/TOML.hpp>, so the
# extras root is the only include path this target publishes.
target_include_directories(zahlen_serialization PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# JSONSchema.hpp and TOML.hpp are the two headers in the tree that reflect
# directly: they walk an aggregate's member list to read and write it.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_serialization)
endif()

target_link_libraries(zahlen_serialization PRIVATE zahlen_warnings)

# --- the six domain targets --------------------------------------------------

add_subdirectory(Animation)
add_subdirectory(Network)
add_subdirectory(ALife)
add_subdirectory(VFX)

# glTF comes after serialization on purpose: GLTFImporter.cpp includes
# <json/JSONSchema.hpp>, and target_link_libraries would silently record a
# library name instead of a dependency edge if the target did not exist yet.
add_subdirectory(glTF)

add_subdirectory(Scripting)
add_subdirectory(editor)
add_subdirectory(Console)

# Optional in a stronger sense than the others: it defines no target at all when
# resvg is not installed. See SVG/CMakeLists.txt.
add_subdirectory(SVG)

# Optional the same way, and for the same reason: no target at all when libcurl
# is not installed. See HTTP/CMakeLists.txt.
add_subdirectory(HTTP)

# --- the aggregate -----------------------------------------------------------
# INTERFACE, because there is nothing left to compile: every source in the six
# directories above belongs to a target of its own. What this target carries is
# the link graph, and through it each domain's PUBLIC include path, module
# interfaces and dependency set.
#
# After this decomposition nothing inside the tree links it: every in-tree
# consumer takes the domain it actually uses, the composition root included (it
# needs SceneTOML and UITOML, so it links zahlen_serialization and no longer
# builds the other five domains' module interfaces at all). What the aggregate is
# for is the out-of-tree consumer that wants all of extras in one link line, and
# -- because it keeps its old name -- the `if(TARGET zahlen_extras)` guards in
# tests/CMakeLists.txt and tests/extras/CMakeLists.txt, which read it as "the
# extras layer was built". A consumer that wants one domain links one domain.
#
# The five targets defined by their own directories above are deliberately not
# aggregated here: zahlen_scripting_lua is a LuaJIT dependency, zahlen_editor is
# gated on ZHLN_HAS_EDITOR, zahlen_console depends on the scripting binder, and
# zahlen_svg / zahlen_http do not exist on a host without their libraries. A
# consumer that wants them links them by name, guarded on TARGET.
add_library(zahlen_extras INTERFACE)

target_link_libraries(zahlen_extras INTERFACE
    zahlen_animation
    zahlen_network
    zahlen_vfx
    zahlen_alife
    zahlen_gltf
    zahlen_serialization
)
