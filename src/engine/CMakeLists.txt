# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# This list is exhaustive on purpose -- nothing here globs. A source the list
# does not name is not compiled, and that failure mode is quiet: the symbols it
# should have defined stay undefined, the shared library still links (undefined
# symbols are allowed there), and only the callers that happen to sit in an
# archive linked into an executable report them. Add the file here in the same
# commit that adds the file; the link error that says otherwise names a target
# two steps away from the omission.

set(RAW_ENGINE_SOURCES
    Log.cpp
    diagnostics/StackTrace.cpp
    diagnostics/MemoryInspector.cpp
    diagnostics/SanitizerBridges.cpp
    diagnostics/RenderDocInit.cpp
    Engine.cpp
    EngineGlobals.cpp
    InputContext.cpp
    KernelSession.cpp
    World.cpp
    VisibilityList.cpp
    system/CharacterMotorSystem.cpp
    com/Engine.cpp
    com/LogicWorld.cpp
    SystemWiring.cpp
    Hierarchy.cpp
    PrefabFactory.cpp
    MeshBuilder.cpp
    Meshlet.cpp
    AssetManager.cpp
    Platform.cpp
    Profiler.cpp
    system/RenderSystem.cpp
    system/DecalSystem.cpp
    system/AudioSystem.cpp
    ArticulationContext.cpp
    system/ArticulationSystem.cpp
    system/CullingSystem.cpp
    system/PhysicsSystem.cpp
    system/LODSystem.cpp
    system/SceneResources.cpp
    system/SceneCleanupSystem.cpp
    FramePlan.cpp
    system/TransformSystem.cpp
    system/PhysicsStateSystem.cpp
    system/LightingSystem.cpp
    system/EnvironmentSunSystem.cpp
    system/CameraSystem.cpp
)

target_sources(zahlen_engine PRIVATE ${RAW_ENGINE_SOURCES})

if(USE_SANITIZERS)
    add_library(zahlen_sanitizer_options OBJECT diagnostics/SanitizerOptions.cpp)
    set_target_properties(zahlen_sanitizer_options PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(zahlen_sanitizer_options PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    )
    # The rules are #embed-ed from the paths exported here, the same way the
    # default font reaches src/gui/FontLoader.cpp: absolute, so the directive
    # cannot drift when a file moves, and never a run-time lookup. Only one
    # level of backslash is needed around the quotes -- "PATH=\"${path}\"" reaches
    # the compiler as -DPATH="/abs/path", which is a string literal; the doubled
    # form emits -DPATH=\\\"/abs/path\\\" and fails to expand (see the note in
    # SanitizerOptions.cpp). OBJECT_DEPENDS is what rebuilds this object when a
    # rule changes: a preprocessor dependency file is not guaranteed to list an
    # #embed-ed file.
    foreach(rule_file IN ITEMS lsan.supp ubsan.supp)
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${rule_file}")
            message(FATAL_ERROR "zahlen_sanitizer_options: '${rule_file}' is missing from the project root")
        endif()
    endforeach()
    # Both definitions travel as one ;-list value: PROPERTIES pairs names with
    # values, so a second bare argument would be read as another property name.
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/SanitizerOptions.cpp" PROPERTIES
        COMPILE_DEFINITIONS "ZHLN_LSAN_SUPPRESSIONS_PATH=\"${PROJECT_SOURCE_DIR}/lsan.supp\";ZHLN_UBSAN_SUPPRESSIONS_PATH=\"${PROJECT_SOURCE_DIR}/ubsan.supp\""
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/lsan.supp;${PROJECT_SOURCE_DIR}/ubsan.supp"
    )
    target_sources(zahlen_engine INTERFACE
        $<BUILD_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:$<TARGET_OBJECTS:zahlen_sanitizer_options>>>
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach(_zhln_san_hook IN ITEMS
            __asan_default_options
            __lsan_default_options
            __lsan_default_suppressions
            __ubsan_default_options
            __ubsan_default_suppressions
            __tsan_default_options)
            target_link_options(zahlen_engine INTERFACE
                "$<BUILD_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:LINKER:--export-dynamic-symbol=${_zhln_san_hook}>>"
            )
        endforeach()
        unset(_zhln_san_hook)
    endif()
endif()

# First-party in-tree host executables on non-Apple, non-sanitizer builds link
# mimalloc directly (BUILD_LOCAL_INTERFACE drops it from installed SDK exports
# so external consumers keep their own process allocator). On ELF, pulling
# mi_malloc_size brings in alloc.c.o (which holds every C/POSIX/glibc/C++
# MI_OVERRIDE=ON wrapper) and --export-dynamic-symbol-list exposes the complete
# allocator surface so the host, libzahlen_engine.so, and dlopen()-loaded
# drivers share one mimalloc heap without leaking internal mi_* symbols.
if(ZHLN_ENABLE_MIMALLOC AND TARGET mimalloc-static)
    target_link_libraries(zahlen_engine INTERFACE
        "$<BUILD_LOCAL_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:mimalloc-static>>"
    )
    if(UNIX AND NOT APPLE)
        set(_zhln_mimalloc_override_list "${PROJECT_SOURCE_DIR}/cmake/mimalloc_override.list")
        target_link_options(zahlen_engine INTERFACE
            "$<BUILD_LOCAL_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:LINKER:--undefined=mi_malloc_size>>"
            "$<BUILD_LOCAL_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:LINKER:--export-dynamic-symbol-list=${_zhln_mimalloc_override_list}>>"
        )
        unset(_zhln_mimalloc_override_list)
    endif()
endif()

target_include_directories(zahlen_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/system
)

add_subdirectory(tty)
