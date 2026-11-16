# Scene documents and capability-only host orchestration. Core never links back.
add_library(zahlen_scene STATIC Scene.cpp)
set_target_properties(zahlen_scene PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(zahlen_scene PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(zahlen_scene PUBLIC zahlen_engine PRIVATE zahlen_warnings)
zahlen_enable_reflection(zahlen_scene)
# Explicit transitional adapter for existing native hosts. Never pulled by COM.
add_library(zahlen_native_scene STATIC NativeScene.cpp)
set_target_properties(zahlen_native_scene PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_native_scene PUBLIC zahlen_scene PRIVATE Jolt zahlen_warnings)
zahlen_enable_reflection(zahlen_native_scene)
