# Installed interfaces for Zahlen engine packages, independent of the OS package
# manager. Nothing here needs privileges: the payload installs into whatever
# prefix the caller owns, and `zpkg` (utils/zpkg) is what governs the per-user
# store under ${XDG_DATA_HOME}/zahlen. The only OS-packaged piece is /usr/bin/zpkg.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Toolchain packaging supports Linux only")
endif()

# The existing engine is shared; optional packages are static. Native exports
# preserve their PRIVATE dependencies as LINK_ONLY, including Serialization.
if(ZHLN_USE_CUSTOM_LIBCXX)
    message(FATAL_ERROR "Toolchain packaging requires the system libstdc++ toolchain")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Toolchain packaging requires a single-config Release build")
endif()
if(NOT ZHLN_HAS_SIMDJSON AND TARGET zahlen_serialization)
    message(FATAL_ERROR "Toolchain packaging requires a system simdjson CONFIG package")
endif()
if(TARGET zahlen_serialization)
    find_package(simdjson CONFIG REQUIRED)
endif()
set(ZHLN_TOOLCHAIN_PROFILE release)
if(USE_SANITIZERS)
    set(ZHLN_TOOLCHAIN_PROFILE asan-ubsan)
endif()
# Capture the standard-library ABI selected by this toolchain, not merely its
# compiler brand (Clang can use either libc++ or libstdc++).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/toolchain-stl.cpp" "#include <string>\n")
separate_arguments(_toolchain_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${_toolchain_flags} -dM -E -x c++
    "${CMAKE_CURRENT_BINARY_DIR}/toolchain-stl.cpp"
    OUTPUT_VARIABLE _toolchain_macros COMMAND_ERROR_IS_FATAL ANY)
foreach(macro __GLIBCXX__ _GLIBCXX_USE_CXX11_ABI)
    if(NOT _toolchain_macros MATCHES "#define ${macro} ([0-9]+)")
        message(FATAL_ERROR "Toolchain packaging requires libstdc++ (${macro} missing)")
    endif()
    set(ZHLN_TOOLCHAIN_${macro} "${CMAKE_MATCH_1}")
endforeach()
if(_toolchain_macros MATCHES "#define _GLIBCXX_DEBUG[ \t\r\n]")
    message(FATAL_ERROR "Toolchain packaging does not support libstdc++ debug containers")
endif()
get_target_property(_toolchain_jolt_defs Jolt INTERFACE_COMPILE_DEFINITIONS)
get_target_property(_toolchain_jolt_options Jolt INTERFACE_COMPILE_OPTIONS)
# Commit identity intentionally makes toolchain upgrades atomic, even before a
# stable ABI exists: a new commit is a new toolchain, never an in-place upgrade.
set(ZHLN_TOOLCHAIN_ABI "${PROJECT_VERSION}-${ZHLN_GIT_HASH}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_SYSTEM_PROCESSOR}-${ZHLN_TOOLCHAIN_PROFILE}")

string(SHA256 ZHLN_TOOLCHAIN_ABI "${ZHLN_TOOLCHAIN_ABI};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE};${_toolchain_jolt_defs};${_toolchain_jolt_options};${ZHLN_NO_ANNOTATION_EXTRACT};${ZHLN_DEV_MODE};${ZHLN_BUILD_RENDER};${ZHLN_TOOLCHAIN___GLIBCXX__};${ZHLN_TOOLCHAIN__GLIBCXX_USE_CXX11_ABI}")

# The human-facing, filesystem-safe toolchain id. It names the store directory,
# the artifact files and the `${prefix}` that zpkg reports. It is derived from
# exactly the inputs hashed above, so an id can never describe two ABIs.
string(TOLOWER "${PROJECT_VERSION}-${ZHLN_GIT_HASH}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_SYSTEM_PROCESSOR}-${ZHLN_TOOLCHAIN_PROFILE}"
    ZHLN_TOOLCHAIN_ID)
set(ZHLN_TOOLCHAIN_METADATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/toolchain")

# Shipped inside every package, so an archive identifies itself no matter what
# it was renamed to on disk. zpkg reads this before it extracts anything.
file(WRITE "${ZHLN_TOOLCHAIN_METADATA_DIR}/toolchain.json"
    "{\"format\":1,\"id\":\"${ZHLN_TOOLCHAIN_ID}\",\"version\":\"${PROJECT_VERSION}\",\"commit\":\"${ZHLN_GIT_HASH}\",\"compiler\":\"${CMAKE_CXX_COMPILER_ID}\",\"compiler_version\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"processor\":\"${CMAKE_SYSTEM_PROCESSOR}\",\"profile\":\"${ZHLN_TOOLCHAIN_PROFILE}\",\"abi\":\"${ZHLN_TOOLCHAIN_ABI}\"}\n")

# Make existing include roots relocatable without changing in-tree behavior.
function(zhln_toolchain_interface target)
    foreach(prop INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
        get_target_property(dirs ${target} ${prop})
        if(dirs)
            set(new_dirs "")
            foreach(dir IN LISTS dirs)
                if(IS_ABSOLUTE "${dir}" AND dir MATCHES "^${PROJECT_SOURCE_DIR}/")
                    list(APPEND new_dirs "$<BUILD_INTERFACE:${dir}>")
                else()
                    list(APPEND new_dirs "${dir}")
                endif()
            endforeach()
            if(NOT prop STREQUAL "INTERFACE_SYSTEM_INCLUDE_DIRECTORIES")
                list(APPEND new_dirs "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
            endif()
            set_property(TARGET ${target} PROPERTY ${prop} "${new_dirs}")
        endif()
    endforeach()
    # Diagnostics are build policy, never a consumer link dependency.
    get_target_property(build_links ${target} LINK_LIBRARIES)
    if(build_links)
        string(REPLACE "zahlen_warnings" "$<BUILD_LOCAL_INTERFACE:zahlen_warnings>" build_links "${build_links}")
        set_property(TARGET ${target} PROPERTY LINK_LIBRARIES "${build_links}")
    endif()
    get_target_property(links ${target} INTERFACE_LINK_LIBRARIES)
    if(links)
        string(REPLACE "$<LINK_ONLY:zahlen_warnings>" "" links "${links}")
        string(REPLACE "zahlen_warnings" "$<BUILD_INTERFACE:zahlen_warnings>" links "${links}")
        set_property(TARGET ${target} PROPERTY INTERFACE_LINK_LIBRARIES "${links}")
    endif()
    target_compile_features(${target} INTERFACE cxx_std_26)
endfunction()

# Machine-readable metadata for zpkg, written here because this file is the only
# place that knows a component's package name, its exported targets and its
# declared dependencies. The old split-package recipe hand-copied that graph into
# a shell array, and the copy drifted from the CMake one. Now there is one
# source and the archives carry it.
function(zhln_toolchain_manifest package component)
    cmake_parse_arguments(M "" "" "REQUIRES;PROVIDES" ${ARGN})
    file(MAKE_DIRECTORY "${ZHLN_TOOLCHAIN_METADATA_DIR}/manifests")
    string(REPLACE ";" "\",\"" _requires "${M_REQUIRES}")
    string(REPLACE ";" "\",\"" _provides "${M_PROVIDES}")
    if(_requires)
        set(_requires "\"${_requires}\"")
    endif()
    if(_provides)
        set(_provides "\"${_provides}\"")
    endif()
    file(WRITE "${ZHLN_TOOLCHAIN_METADATA_DIR}/manifests/${package}.json"
        "{\"format\":1,\"package\":\"${package}\",\"component\":\"${component}\",\"toolchain\":\"${ZHLN_TOOLCHAIN_ID}\",\"profile\":\"${ZHLN_TOOLCHAIN_PROFILE}\",\"requires\":[${_requires}],\"provides\":[${_provides}]}\n")
    # Every package carries the toolchain descriptor so an archive identifies
    # itself wherever it ends up. zpkg refuses a toolchain it cannot name.
    install(FILES "${ZHLN_TOOLCHAIN_METADATA_DIR}/toolchain.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen COMPONENT Zahlen${component})
    install(FILES "${ZHLN_TOOLCHAIN_METADATA_DIR}/manifests/${package}.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen/manifests COMPONENT Zahlen${component})
endfunction()

# One export set and install component per package. Dependencies are explicit;
# CMake itself checks that every linked non-imported target is exported.
function(zhln_toolchain_package name)
    cmake_parse_arguments(P "" "" "TARGETS;DEPENDS;HEADERS" ${ARGN})
    list(GET P_TARGETS 0 first_target)
    if(NOT TARGET ${first_target})
        return()
    endif()
    set(ZHLN_TOOLCHAIN_DEPENDENCIES "")
    foreach(dep IN LISTS P_DEPENDS)
        string(APPEND ZHLN_TOOLCHAIN_DEPENDENCIES "find_dependency(Zahlen${dep} ${PROJECT_VERSION} EXACT CONFIG)\n")
    endforeach()
    if(name STREQUAL "Serialization")
        string(APPEND ZHLN_TOOLCHAIN_DEPENDENCIES "find_dependency(simdjson CONFIG)\n")
    endif()
    foreach(target IN LISTS P_TARGETS)
        zhln_toolchain_interface(${target})
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
        install(TARGETS ${target} EXPORT Zahlen${name}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Zahlen${name}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Zahlen${name}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Zahlen${name}
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zahlen/modules
                COMPONENT Zahlen${name})
    endforeach()
    foreach(dir IN LISTS P_HEADERS)
        set(header_dest "${CMAKE_INSTALL_INCLUDEDIR}")
        if(NOT dir STREQUAL "include" AND NOT dir STREQUAL "app/include")
            get_filename_component(leaf "${dir}" NAME)
            string(APPEND header_dest "/${leaf}")
        endif()
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/${dir}/"
            DESTINATION ${header_dest} COMPONENT Zahlen${name}
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "*.inl")
    endforeach()
    set(dest ${CMAKE_INSTALL_LIBDIR}/cmake/Zahlen${name})
    set(ZHLN_TOOLCHAIN_PACKAGE ${name})
    configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/toolchain/PackageConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/toolchain/Zahlen${name}Config.cmake
        INSTALL_DESTINATION ${dest})
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/toolchain/Zahlen${name}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/toolchain/Zahlen${name}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/toolchain/Zahlen${name}ConfigVersion.cmake
        DESTINATION ${dest} COMPONENT Zahlen${name})
    install(EXPORT Zahlen${name}Targets NAMESPACE Zahlen::
        DESTINATION ${dest} COMPONENT Zahlen${name}
        CXX_MODULES_DIRECTORY cxx-modules)
    # The manifest states the dependency graph once. zpkg reads it from the
    # archive, so nothing else has to keep a second copy of it.
    string(TOLOWER "${name}" _package_leaf)
    set(_requires "")
    foreach(dep IN LISTS P_DEPENDS)
        string(TOLOWER "${dep}" _dep_leaf)
        list(APPEND _requires "zahlen-${_dep_leaf}")
    endforeach()
    set(_provides "")
    foreach(target IN LISTS P_TARGETS)
        get_target_property(_export_name ${target} EXPORT_NAME)
        if(NOT _export_name)
            set(_export_name ${target})
        endif()
        list(APPEND _provides "Zahlen::${_export_name}")
    endforeach()
    zhln_toolchain_manifest("zahlen-${_package_leaf}" "${name}"
        REQUIRES ${_requires} PROVIDES ${_provides})
endfunction()

# Public installed names, including helper libraries needed by static consumers.
foreach(pair "engine;Engine" "application;Application" "host_runner;HostRunner"
        "command_line;CommandLine" "signal;Signal" "gltf;GLTF"
        "netpbm;Netpbm" "screenshot;Screenshot" "serialization;Serialization" "ui_schema;UISchema"
        "animation;Animation" "skeletal_animation;SkeletalAnimation"
        "character_controller;CharacterController" "camera;Camera" "freecam;FreeCam")
    list(GET pair 0 internal)
    list(GET pair 1 public)
    if(TARGET zahlen_${internal})
        set_target_properties(zahlen_${internal} PROPERTIES EXPORT_NAME ${public})
    endif()
endforeach()
set_target_properties(Jolt PROPERTIES EXPORT_NAME Jolt)
if(USE_SANITIZERS)
    target_compile_options(zahlen_engine INTERFACE -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    target_link_options(zahlen_engine INTERFACE -fsanitize=address,undefined -fno-sanitize=vptr)
    target_compile_definitions(zahlen_engine INTERFACE __ASAN_ENABLED__ ZHLN_SANITIZER_BUILD=1)
endif()
if(ZHLN_DEV_MODE)
    target_compile_definitions(zahlen_engine INTERFACE ZHLN_DEV_MODE)
endif()
if(ZHLN_NO_ANNOTATION_EXTRACT)
    target_compile_definitions(zahlen_engine INTERFACE ZHLN_NO_ANNOTATION_EXTRACT)
endif()
zhln_toolchain_package(Engine TARGETS zahlen_engine Jolt HEADERS include)
# The sanitizer option hooks must be linked into the consumer's executable:
# libasan's weak stubs shadow anything defined inside libzahlen_engine.so, and
# an exported INTERFACE source cannot carry a relocatable path, so the object is
# installed next to the toolchain and ZahlenEngineConfig.cmake attaches it. The rules
# are already compiled into this object; there is no runtime file to find.
if(TARGET zahlen_sanitizer_options)
    install(FILES $<TARGET_OBJECTS:zahlen_sanitizer_options>
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/zahlen
        RENAME zahlen_sanitizer_options.o
        COMPONENT ZahlenEngine)
endif()
# Jolt's headers must retain their Jolt/ prefix.
install(DIRECTORY extern/JoltPhysics/Jolt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT ZahlenEngine FILES_MATCHING PATTERN "*.h" PATTERN "*.inl")
zhln_toolchain_package(CommandLine TARGETS zahlen_command_line HEADERS extensions/CommandLine)
zhln_toolchain_package(Signal TARGETS zahlen_signal DEPENDS Engine HEADERS extensions/Signal)
zhln_toolchain_package(HostRunner TARGETS zahlen_host_runner DEPENDS Engine CommandLine Signal)
zhln_toolchain_package(Application TARGETS zahlen_application DEPENDS Engine CommandLine HostRunner)
# Consumer-side header only: install independently of the in-tree Application
# package so COM clients need not link its static libraries.
install(FILES app/include/Zahlen/COM/App.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Zahlen/COM COMPONENT ZahlenCOMApp)
install(FILES app/include/Zahlen/HostRunner.hpp app/include/Zahlen/KernelApp.hpp
    app/include/Zahlen/AppOutcome.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Zahlen
    COMPONENT ZahlenHostRunner)
install(FILES app/include/Zahlen/App.hpp app/include/Zahlen/ConfigCli.hpp
    app/include/Zahlen/Zahlen.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Zahlen
    COMPONENT ZahlenApplication)
zhln_toolchain_package(Netpbm TARGETS zahlen_netpbm DEPENDS Engine HEADERS plugins/netpbm)
zhln_toolchain_package(Screenshot TARGETS zahlen_screenshot DEPENDS Engine HEADERS extensions/Screenshot)
zhln_toolchain_package(UISchema TARGETS zahlen_ui_schema DEPENDS Engine HEADERS extensions/UI)
# UISchema is a HEADER dependency of toml/UITOML.hpp, not a link in the
# exported interface: the installed extensions/UI/*.hpp must exist for a
# consumer to compile that header, so the package still declares it.
zhln_toolchain_package(Serialization TARGETS zahlen_serialization DEPENDS Engine UISchema HEADERS plugins/json plugins/toml)
zhln_toolchain_package(GLTF TARGETS zahlen_gltf DEPENDS Engine Serialization HEADERS plugins/glTF)
zhln_toolchain_package(FreeCam TARGETS zahlen_freecam DEPENDS Engine HEADERS extensions/FreeCam)
zhln_toolchain_package(Camera TARGETS zahlen_camera DEPENDS Engine HEADERS extensions/Camera)
zhln_toolchain_package(CharacterController TARGETS zahlen_character_controller DEPENDS Engine HEADERS extensions/CharacterController)
zhln_toolchain_package(SkeletalAnimation TARGETS zahlen_skeletal_animation DEPENDS Engine HEADERS extensions/Animation)
zhln_toolchain_package(Animation TARGETS zahlen_animation DEPENDS Engine SkeletalAnimation CharacterController Camera FreeCam)
# Serialization is a PRIVATE link of the plugin, which the export rewrites to
# $<LINK_ONLY:...>; the exported interface still names Zahlen::Serialization, so
# the package has to declare it or the consumer's find_package() fails on it.
zhln_toolchain_package(Fonts TARGETS zahlen_fonts DEPENDS Engine Serialization HEADERS plugins/Fonts)
# A font package with no font in it leaves the consumer back on the compiled-in
# 32 px raster, which is exactly the state this package exists to end: the plugin
# owns the fontbm .fnt + atlas path, and these 50 KB are what it feeds. The
# vendored faces are licence-clean and already in the tree, so they travel with
# the code that parses them.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/resources/fonts/JetBrainsMonoNerdFontRegular/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen/fonts/JetBrainsMonoNerdFontRegular
    COMPONENT ZahlenFonts)

# --- Tools, not content ----------------------------------------------------
# There is deliberately no Data package. What used to be one was the sandbox's
# cooked pack plus the whole resources/ tree, and a consumer needs neither: the
# renderer #embeds its shaders (ShaderCompilation.cmake), src/gui/FontLoader.cpp
# #embeds the default font, and Kernel.cpp only logs a warning when
# data/base.pak is absent. A pack belongs to whoever owns the scene -- this
# repository cooks one for its own samples, and an application cooks its own.
#
# Shipping it also coupled every toolchain build to the asset pipeline: the
# install rule copied a custom-target artifact, so
# `cmake --install --component ZahlenData` could not stand on its own and failed
# with a COPY_FILE error from generated install code whenever the cooker had not
# run in that tree.
#
# What a consumer does need is the tool that makes content, so zcook ships
# instead, with the one engine script the cook graph names -- it is required even
# in a Blender-free graph. zcook links the engine, hence $ORIGIN/../lib rather
# than the $ORIGIN the libraries use, and a relative rpath because the store
# relocates.
if(TARGET zcook)
    set_target_properties(zcook PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS zcook RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ZahlenCooker)
    install(FILES ${PROJECT_SOURCE_DIR}/scripts/export_metadata.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen/tools COMPONENT ZahlenCooker)
    zhln_toolchain_manifest(zahlen-cooker Cooker
        REQUIRES zahlen-engine PROVIDES zcook)
endif()
