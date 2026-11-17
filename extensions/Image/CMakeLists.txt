# Engine-independent build-tool and consumer codec; never link Core here.
add_library(zahlen_image STATIC Image.cpp)
set_target_properties(zahlen_image PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(zahlen_image PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/extensions>)
target_include_directories(zahlen_image SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/extern/stb)
target_compile_options(zahlen_image PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
