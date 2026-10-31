cmake_minimum_required(VERSION 3.28)
project(installed_cube LANGUAGES CXX)
find_package(ZahlenApplication CONFIG REQUIRED)
find_package(ZahlenNetpbm CONFIG REQUIRED)
if(TARGET Zahlen::Animation OR TARGET Zahlen::CharacterController)
    message(FATAL_ERROR "Application pulled optional gameplay into the consumer")
endif()
# Both source files belong to this standalone consumer project.
# No engine source files, source paths, or build-tree targets are used.
add_executable(cube main.cpp InstalledCubeSmoke.cpp)
target_link_libraries(cube PRIVATE Zahlen::Application Zahlen::Netpbm)
