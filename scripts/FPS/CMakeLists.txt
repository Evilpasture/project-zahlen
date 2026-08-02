cmake_minimum_required(VERSION 3.28)

add_library(zahlen_fps STATIC)

set_target_properties(zahlen_fps PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON
    CXX_MODULE_STD ON
)

target_sources(zahlen_fps
    PUBLIC
        FILE_SET CXX_MODULES FILES
            FPS.cppm
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ZAHLEN_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to Zahlen Engine SDK")
    target_include_directories(zahlen_fps PUBLIC
        ${ZAHLEN_SDK_DIR}/include
        ${ZAHLEN_SDK_DIR}/src
        ${ZAHLEN_SDK_DIR}/extern/JoltPhysics
    )
else()
    target_include_directories(zahlen_fps PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
    )
endif()

target_compile_definitions(zahlen_fps PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_SHARED_LIBRARY
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:JPH_RELEASE>
)

if(NOT MSVC)
    target_compile_options(zahlen_fps PRIVATE
        -Wall -Wextra -Wpedantic
        -fno-exceptions
        -fno-rtti
        $<$<CXX_COMPILER_ID:GNU>:-fmodules-ts>
    )
endif()

target_link_libraries(zahlen_fps PUBLIC Jolt)
