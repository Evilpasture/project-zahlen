# src/ecs/CMakeLists.txt

add_library(zahlen_ecs OBJECT
    ECS.cpp
    ECS.hpp
    SystemGraph.cpp
    SystemGraph.hpp
    EntityCommandBuffer.cpp
    EntityCommandBuffer.hpp
)
set_target_properties(zahlen_ecs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(zahlen_ecs PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(NOT MSVC)
    target_compile_options(zahlen_ecs PRIVATE -fno-exceptions -fno-rtti)
endif()
