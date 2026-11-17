# Combat, Lightning and Explosion policies are ordinary COM host objects.
add_library(zahlen_vfx STATIC CombatFXLifecycle.cpp LightningGeometry.cpp LightningHost.cpp ExplosionHost.cpp)
set_target_properties(zahlen_vfx PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_SCAN_FOR_MODULES ON POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_vfx PUBLIC zahlen_engine PRIVATE zahlen_warnings)
target_include_directories(zahlen_vfx PUBLIC ${PROJECT_SOURCE_DIR}/extensions)
zahlen_enable_reflection(zahlen_vfx)

# Explicit unported native callers retain the legacy module/service adapters.
# These are not linked into the COM implementation.
add_library(zahlen_native_vfx STATIC Lightning.cpp LightningUpdate.cpp Explosion.cpp ExplosionLifecycle.cpp)
set_target_properties(zahlen_native_vfx PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_SCAN_FOR_MODULES ON POSITION_INDEPENDENT_CODE ON)
target_sources(zahlen_native_vfx PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${PROJECT_SOURCE_DIR}/extensions FILES Explosion.cppm Lightning.cppm)
target_link_libraries(zahlen_native_vfx PUBLIC zahlen_engine PRIVATE zahlen_vfx PRIVATE zahlen_warnings)
target_include_directories(zahlen_native_vfx PUBLIC ${PROJECT_SOURCE_DIR}/extensions)
zahlen_enable_reflection(zahlen_native_vfx)
