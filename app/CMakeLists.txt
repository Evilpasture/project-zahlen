# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Process/pace policy and the Kernel-only host are shared by the game and
# standalone 2D tools. This target has no optional gameplay dependencies.
add_library(zahlen_host_runner STATIC HostRunner.cpp KernelApp.cpp)
set_target_properties(zahlen_host_runner PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(zahlen_host_runner
    PUBLIC zahlen_engine_cli
    PRIVATE zahlen_signal zahlen_warnings
)
# Only app-owned targets publish the host-facing headers under app/include/.
target_include_directories(zahlen_host_runner
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(zahlen_host_runner PRIVATE JPH_DOUBLE_PRECISION)
zahlen_enable_reflection(zahlen_host_runner)

# Application composition lives outside Core: ordinary games link just this
# target, while bespoke hosts can still use zahlen_engine directly. CMake
# writes only the default-module includes/calls present in this build graph.
add_library(zahlen_application STATIC Application.cpp)
set_target_properties(zahlen_application PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
# App.hpp exposes CommandHandler/CommandLineOptions, so its CLI include root
# must reach ordinary games through the application target's public interface.
target_link_libraries(zahlen_application
    PUBLIC zahlen_engine zahlen_engine_cli
    PRIVATE zahlen_host_runner zahlen_warnings
)
target_include_directories(zahlen_application
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/plugins
        ${PROJECT_SOURCE_DIR}/extensions
        ${PROJECT_SOURCE_DIR}/gameplay
)
target_compile_definitions(zahlen_application PRIVATE JPH_DOUBLE_PRECISION)
zahlen_enable_reflection(zahlen_application)

# Generate the ordered default-module install pass from build targets, not
# compiler definitions. No disabled extension can leak an include or symbol
# reference into this translation unit (including extras OFF).
set(AUTOLOAD_INCLUDES "")
set(AUTOLOAD_CALLS "")

if(TARGET zahlen_fonts)
    target_link_libraries(zahlen_application PRIVATE zahlen_fonts)
    string(APPEND AUTOLOAD_INCLUDES "#include <Fonts/Fonts.hpp>\n")
    string(APPEND AUTOLOAD_CALLS
        "    // Prime the cache before InitializeDefaultScene uploads the font atlas.\n"
        "    if (auto fontID = Fonts::LoadFontAsset(engine, Fonts::VendoredDefaultFontSource()); !fontID) {\n"
        "        LogWarning(\"Font asset failed to load ({}), using embedded default.\", fontID.error());\n"
        "    }\n"
    )
endif()
if(TARGET zahlen_character_controller)
    target_link_libraries(zahlen_application PRIVATE zahlen_character_controller)
    string(APPEND AUTOLOAD_INCLUDES "#include <CharacterController/CharacterController.hpp>\n")
    string(APPEND AUTOLOAD_CALLS "    // Character writes MovementComponent; Interaction reads it.\n"
                                 "    Character::Install(engine);\n")
endif()
if(TARGET zahlen_camera)
    target_link_libraries(zahlen_application PRIVATE zahlen_camera)
    string(APPEND AUTOLOAD_INCLUDES "#include <Camera/TargetCamera.hpp>\n")
    string(APPEND AUTOLOAD_CALLS "    CameraRig::Install(engine);\n")
endif()
if(TARGET zahlen_interaction)
    target_link_libraries(zahlen_application PRIVATE zahlen_interaction)
    string(APPEND AUTOLOAD_INCLUDES "#include <Interaction/InteractionSystem.hpp>\n")
    string(APPEND AUTOLOAD_CALLS "    Interaction::Install(engine);\n")
endif()
if(TARGET zahlen_terrain)
    target_link_libraries(zahlen_application PRIVATE zahlen_terrain)
    string(APPEND AUTOLOAD_INCLUDES "#include <Terrain/TerrainSystem.hpp>\n")
    string(APPEND AUTOLOAD_CALLS "    Terrain::Install(engine);\n")
endif()
if(TARGET zahlen_animation)
    target_link_libraries(zahlen_application PRIVATE zahlen_animation)
    string(APPEND AUTOLOAD_INCLUDES "#include <Animation/IK.hpp>\n")
    string(APPEND AUTOLOAD_CALLS "    IK::Install(engine); // installs Animation before IK\n")
endif()
if(TARGET zahlen_freecam)
    target_link_libraries(zahlen_application PRIVATE zahlen_freecam)
    string(APPEND AUTOLOAD_INCLUDES "#include <FreeCam/FreeCam.hpp>\n")
    string(APPEND AUTOLOAD_CALLS "    FreeCam::Install(engine);\n")
endif()
if(TARGET zahlen_ragdoll_authoring)
    target_link_libraries(zahlen_application PRIVATE zahlen_ragdoll_authoring)
    string(APPEND AUTOLOAD_INCLUDES "#include <RagdollAuthoring/RagdollAuthoring.hpp>\n")
    string(APPEND AUTOLOAD_CALLS "    RagdollAuthoring::Install(engine);\n")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/DefaultModules.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DefaultModules.cpp"
    @ONLY
)
target_sources(zahlen_application PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/DefaultModules.cpp")

# The UI-tree editor uses the Kernel-only runner, not application gameplay.
if(TARGET zahlen_ui_editor)
    target_link_libraries(zahlen_ui_editor PRIVATE zahlen_host_runner)
endif()
