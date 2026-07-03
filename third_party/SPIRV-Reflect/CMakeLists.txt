cmake_minimum_required(VERSION 3.15)
project(spirv_reflect C)

add_library(spirv_reflect STATIC spirv_reflect.c)

target_include_directories(spirv_reflect 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(spirv_reflect PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
