add_library(zahlen_threading STATIC
    Thread.cpp
    Thread.S
    Mutex.cpp
    Mutex.hpp
    Thread.hpp
    TaskSystem.cpp
    TaskSystem.hpp
)
set_target_properties(zahlen_threading PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Allow other modules to find the headers
target_include_directories(zahlen_threading PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Enable ASM support if not already inherited
set_target_properties(zahlen_threading PROPERTIES LINKER_LANGUAGE CXX)

if(NOT MSVC)
    target_compile_options(zahlen_threading PRIVATE -fno-exceptions -fno-rtti)
endif()