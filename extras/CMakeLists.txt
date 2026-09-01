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
# The two serialisation extras are plain headers plus a translation unit each:
#
#   extras/json/   JSON.hpp, JSONSchema.hpp  reflection-driven JSON (simdjson)
#   extras/toml/   TOML.hpp, SceneTOML.hpp   reflection-driven TOML, and the
#                                            binding that reads/writes a core
#                                            ZHLN::Scene::Scene as a document
#   extras/glTF/   the glTF/GLB importer (GLTFImporter.*) plus the drop-a-file
#                  inspector (ZHLN.glTF). The importer is the one extra that
#                  produces data core then consumes: it fills the prefab cache,
#                  and CreativeWorksFactory::LoadModelPrefab reads that cache.
#                  No registration, and nothing in src/ includes it.
#
# All of it is picked up by the source globs below; nothing needs listing by
# hand.

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

# The Network stack serializes through ZHLN.Wire (C++26 reflection + std::expected,
# see extras/Network/Wire.cppm): no msgpack, no zlib, and — critically — no
# per-file -fexceptions overrides. Mixed exception dialects between a module
# interface and its importers make GCC reject the compiled BMI ("language
# dialect differs"); every unit of ZHLN.* now builds -fno-exceptions.

# 1. Discover all C++26 module interface files (.cppm)
file(GLOB_RECURSE ZHLN_EXTRAS_MODULES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
)

# extras/scripting_lua is its own target: it is the only extra that needs
# LuaJIT, and folding it into zahlen_extras would make every consumer of the
# extras archive link a Lua runtime it may not want.
list(FILTER ZHLN_EXTRAS_MODULES EXCLUDE REGEX ".*/extras/scripting_lua/.*")

# 2. Discover all implementation source files (.cpp)
file(GLOB_RECURSE ZHLN_EXTRAS_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
list(FILTER ZHLN_EXTRAS_SOURCES EXCLUDE REGEX ".*/extras/scripting_lua/.*")

add_library(zahlen_extras STATIC)

set_target_properties(zahlen_extras PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE. It matters here because the glTF
    # importer arrived with extern/cgltf_impl.c, a C translation unit that
    # samples and tests link straight into a PIE executable.
    POSITION_INDEPENDENT_CODE ON
)

if(ZHLN_EXTRAS_MODULES)
    set_source_files_properties(${ZHLN_EXTRAS_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    target_sources(zahlen_extras
        PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES ${ZHLN_EXTRAS_MODULES}
    )
endif()

if(ZHLN_EXTRAS_SOURCES)
    target_sources(zahlen_extras PRIVATE ${ZHLN_EXTRAS_SOURCES})
endif()

# extras/glTF/GLTFImporter.cpp is the glTF/GLB reader, and it is the reason
# this target carries more than a header path. Core never links or calls it --
# it only reads the prefabs it leaves in the cache -- so everything the reader
# needs stays here:
#
#   * cgltf      -- the container parser, header plus this one impl TU. It used
#                   to live in the src/gltf static library; nothing else in the
#                   tree defines it, so exactly one copy is compiled.
#   * stb_image  -- header only. STB_IMAGE_IMPLEMENTATION is extern/stbi_impl.c,
#                   which is part of zahlen_engine, so the decode symbols arrive
#                   through the engine link below.
#   * meshoptimizer -- Zahlen/Meshlet.hpp's inline BuildMeshlets() calls into
#                   it; zahlen_engine links it PRIVATE, so it does not propagate.
target_sources(zahlen_extras PRIVATE ${PROJECT_SOURCE_DIR}/extern/cgltf_impl.c)

target_include_directories(zahlen_extras SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/cgltf
    ${PROJECT_SOURCE_DIR}/extern/stb
)

target_link_libraries(zahlen_extras PRIVATE meshoptimizer)

target_include_directories(zahlen_extras PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# simdjson is an implementation detail of extras/json/JSON.cpp -- json/JSON.hpp
# exposes an opaque reader and never includes <simdjson.h> -- so the include
# path and the library both stay PRIVATE and cannot leak into a consumer.
if(SIMDJSON_INCLUDE_DIRS)
    target_include_directories(zahlen_extras PRIVATE ${SIMDJSON_INCLUDE_DIRS})
endif()
if(TARGET PkgConfig::SIMDJSON)
    target_link_libraries(zahlen_extras PRIVATE PkgConfig::SIMDJSON)
endif()

target_link_libraries(zahlen_extras PUBLIC
    zahlen_engine
)

target_link_libraries(zahlen_extras PRIVATE zahlen_warnings)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_extras)
endif()

add_subdirectory(scripting_lua)
