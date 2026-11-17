# Capability-only host terrain; no module, global slot table or graph adapter.
add_library(zahlen_terrain STATIC TerrainGeometry.cpp TerrainHost.cpp)
set_target_properties(zahlen_terrain PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_terrain PUBLIC zahlen_engine PRIVATE zahlen_warnings)
target_include_directories(zahlen_terrain PUBLIC ${PROJECT_SOURCE_DIR}/extensions)
zahlen_enable_reflection(zahlen_terrain)

# Explicit unported native callers retain their existing lifecycle and API.
add_library(zahlen_native_terrain STATIC TerrainFactory.cpp TerrainSystem.cpp)
set_target_properties(zahlen_native_terrain PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_native_terrain PUBLIC zahlen_terrain PRIVATE zahlen_warnings)
zahlen_enable_reflection(zahlen_native_terrain)
