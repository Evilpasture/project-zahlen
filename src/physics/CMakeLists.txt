add_library(zahlen_physics STATIC
    Physics.cpp
    Physics.hpp
    PhysicsWorld.hpp
    PhysicsContactEvents.hpp
)

target_link_libraries(zahlen_physics 
    PRIVATE 
        Jolt 
        zahlen_threading
)

target_include_directories(zahlen_physics PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Pass Jolt specific configurations down
target_compile_definitions(zahlen_physics PRIVATE 
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

if(NOT MSVC)
    target_compile_options(zahlen_physics PRIVATE -fno-exceptions -fno-rtti)
endif()