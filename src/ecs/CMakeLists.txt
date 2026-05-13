add_library(zahlen_ecs STATIC
    ECS.cpp
    ECS.hpp
)


target_include_directories(zahlen_ecs PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)


if(NOT MSVC)
    target_compile_options(zahlen_ecs PRIVATE -fno-exceptions -fno-rtti)
endif()
