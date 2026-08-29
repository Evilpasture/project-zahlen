# extras/CMakeLists.txt
option(ZHLN_BUILD_EXTRAS "Build first-party engine feature modules and VFX toolkits" ON)

if(NOT ZHLN_BUILD_EXTRAS)
    return()
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

# 2. Discover all implementation source files (.cpp)
file(GLOB_RECURSE ZHLN_EXTRAS_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(zahlen_extras STATIC)

set_target_properties(zahlen_extras PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
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

target_include_directories(zahlen_extras PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(zahlen_extras PUBLIC
    zahlen_engine
)

target_link_libraries(zahlen_extras PRIVATE zahlen_warnings)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_extras)
endif()
