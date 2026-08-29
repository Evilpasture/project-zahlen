# extras/CMakeLists.txt
option(ZHLN_BUILD_EXTRAS "Build first-party engine feature modules and VFX toolkits" ON)

if(NOT ZHLN_BUILD_EXTRAS)
    return()
endif()

find_package(ZLIB REQUIRED)

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

# Enable exceptions specifically for Network implementation files to let msgpack compile internally
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Network/NetworkReplicator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Network/NetworkClient.cpp"
    PROPERTIES COMPILE_OPTIONS "-fexceptions"
)

target_include_directories(zahlen_extras PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(zahlen_extras SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../extern/msgpack-c/include
)

target_link_libraries(zahlen_extras PUBLIC
    zahlen_engine
    ZLIB::ZLIB
)

target_link_libraries(zahlen_extras PRIVATE zahlen_warnings)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_extras)
endif()
