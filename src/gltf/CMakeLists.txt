# src/gltf/CMakeLists.txt
add_library(zahlen_gltf STATIC
    GLTFImporter.cpp
    ${CMAKE_SOURCE_DIR}/extern/cgltf_impl.c
)
set_target_properties(zahlen_gltf PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(zahlen_gltf PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
)

target_link_libraries(zahlen_gltf PRIVATE
    zahlen_render
    zahlen_physics
    zahlen_threading
    Jolt
)

if(NOT MSVC)
    target_compile_options(zahlen_gltf PRIVATE -fno-exceptions -fno-rtti)
endif()
