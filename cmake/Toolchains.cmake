# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Installed interfaces for Zahlen engine packages, independent of the OS package
# manager. ZahlenEngine ships the single shared library (libzahlen_engine.so)
# and the stripped public COM/Core headers; all optional extensions and plugins
# ship as relocatable source/header packages compiled by the consumer's own
# toolchain against Zahlen::Engine (zero prebuilt .a archives, zero Jolt leaks).
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Toolchain packaging supports Linux only")
endif()
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
if(USE_SANITIZERS)
    message(FATAL_ERROR
        "Toolchain packaging is release-only; configure with -DUSE_SANITIZERS=OFF")
endif()

# Packaged toolchains are release builds only. Keep the profile in metadata for
# consumers that inspect toolchain descriptors, but do not expose a consumer-side
# profile switch.
set(ZHLN_TOOLCHAIN_PROFILE release)

# Verify libstdc++ dual-ABI setting and forbid debug containers. Deliberately
# ignore __GLIBCXX__ release date, exact compiler patch version, and git commit
# hash so packages travel across Linux distros that meet the glibc/GLIBCXX floor.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/toolchain-stl.cpp" "#include <string>\n")
separate_arguments(_toolchain_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${_toolchain_flags} -dM -E -x c++
    "${CMAKE_CURRENT_BINARY_DIR}/toolchain-stl.cpp"
    OUTPUT_VARIABLE _toolchain_macros COMMAND_ERROR_IS_FATAL ANY)
if(NOT _toolchain_macros MATCHES "#define _GLIBCXX_USE_CXX11_ABI ([0-9]+)")
    message(FATAL_ERROR "Toolchain packaging requires libstdc++ (_GLIBCXX_USE_CXX11_ABI missing)")
endif()
set(ZHLN_TOOLCHAIN__GLIBCXX_USE_CXX11_ABI "${CMAKE_MATCH_1}")
if(_toolchain_macros MATCHES "#define _GLIBCXX_DEBUG[ \t\r\n]")
    message(FATAL_ERROR "Toolchain packaging does not support libstdc++ debug containers")
endif()

# Stable cross-distro ABI identity: project version + compiler family + CPU +
# release profile + binary-affecting engine switches (no compiler patch
# version, no git hash, no __GLIBCXX__ date, no Jolt flags).
string(TOLOWER "${PROJECT_VERSION}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_SYSTEM_PROCESSOR}-${ZHLN_TOOLCHAIN_PROFILE}"
    ZHLN_TOOLCHAIN_ID)
string(SHA256 ZHLN_TOOLCHAIN_ABI
    "${ZHLN_TOOLCHAIN_ID};cxx11_abi=${ZHLN_TOOLCHAIN__GLIBCXX_USE_CXX11_ABI};no_annot=${ZHLN_NO_ANNOTATION_EXTRACT};dev_in_pkg=${ZHLN_DEV_MODE_IN_INSTALLED_PACKAGES};render=${ZHLN_BUILD_RENDER}")

set(ZHLN_TOOLCHAIN_METADATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/toolchain")
file(MAKE_DIRECTORY "${ZHLN_TOOLCHAIN_METADATA_DIR}")

# Shipped inside every package; commit and compiler_version stay recorded as
# provenance metadata without locking the ABI hash or package id.
file(WRITE "${ZHLN_TOOLCHAIN_METADATA_DIR}/toolchain.json"
    "{\"format\":1,\"id\":\"${ZHLN_TOOLCHAIN_ID}\",\"version\":\"${PROJECT_VERSION}\",\"commit\":\"${ZHLN_GIT_HASH}\",\"compiler\":\"${CMAKE_CXX_COMPILER_ID}\",\"compiler_version\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"processor\":\"${CMAKE_SYSTEM_PROCESSOR}\",\"profile\":\"${ZHLN_TOOLCHAIN_PROFILE}\",\"abi\":\"${ZHLN_TOOLCHAIN_ABI}\"}\n")

string(REGEX MATCH "^[0-9]+" ZHLN_TOOLCHAIN_COMPILER_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
if(NOT ZHLN_TOOLCHAIN_COMPILER_MAJOR)
    message(FATAL_ERROR "Cannot determine compiler major version for ZahlenToolchain.cmake")
endif()
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/toolchain/ZahlenToolchain.cmake.in"
    "${ZHLN_TOOLCHAIN_METADATA_DIR}/ZahlenToolchain.cmake"
    @ONLY)
install(FILES "${ZHLN_TOOLCHAIN_METADATA_DIR}/ZahlenToolchain.cmake"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/zahlen" COMPONENT ZahlenEngine)

function(zhln_toolchain_interface target)
    foreach(prop INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
        get_target_property(dirs ${target} ${prop})
        if(dirs)
            set(new_dirs "")
            foreach(dir IN LISTS dirs)
                # Two build-tree-only directories: the source headers, and the
                # generated ones (include/Zahlen/Version.hpp). Both are spelled
                # $<BUILD_INTERFACE:...> so the exported target never names this
                # machine's paths; the installed copy comes from INSTALL_INTERFACE.
                if(IS_ABSOLUTE "${dir}" AND (dir MATCHES "^${PROJECT_SOURCE_DIR}/"
                                             OR dir MATCHES "^${ZHLN_GENERATED_INCLUDE_DIR}(/|$)"))
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
    install(FILES "${ZHLN_TOOLCHAIN_METADATA_DIR}/toolchain.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen COMPONENT Zahlen${component})
    install(FILES "${ZHLN_TOOLCHAIN_METADATA_DIR}/manifests/${package}.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen/manifests COMPONENT Zahlen${component})
endfunction()

function(_zhln_write_package_config name depends)
    set(ZHLN_TOOLCHAIN_DEPENDENCIES "")
    foreach(dep IN LISTS depends)
        string(APPEND ZHLN_TOOLCHAIN_DEPENDENCIES "find_dependency(Zahlen${dep} ${PROJECT_VERSION} CONFIG)\n")
    endforeach()
    if(name STREQUAL "Serialization")
        string(APPEND ZHLN_TOOLCHAIN_DEPENDENCIES "find_dependency(simdjson CONFIG)\n")
    endif()
    if(name STREQUAL "Lua")
        string(APPEND ZHLN_TOOLCHAIN_DEPENDENCIES [=[
if(NOT TARGET LuaJIT)
    find_library(_zahlen_luajit NAMES luajit-5.1 luajit REQUIRED)
    find_path(_zahlen_luajit_inc NAMES lua.hpp PATH_SUFFIXES luajit-2.1 luajit-2.0 REQUIRED)
    add_library(LuaJIT INTERFACE IMPORTED)
    set_property(TARGET LuaJIT PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_zahlen_luajit_inc}")
    set_property(TARGET LuaJIT PROPERTY INTERFACE_LINK_LIBRARIES "${_zahlen_luajit};${CMAKE_DL_LIBS};m")
endif()
]=])
    endif()
    set(dest ${CMAKE_INSTALL_LIBDIR}/cmake/Zahlen${name})
    set(ZHLN_TOOLCHAIN_PACKAGE ${name})
    configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/toolchain/PackageConfig.cmake.in
        ${ZHLN_TOOLCHAIN_METADATA_DIR}/Zahlen${name}Config.cmake
        INSTALL_DESTINATION ${dest})
    write_basic_package_version_file(${ZHLN_TOOLCHAIN_METADATA_DIR}/Zahlen${name}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES
        ${ZHLN_TOOLCHAIN_METADATA_DIR}/Zahlen${name}Config.cmake
        ${ZHLN_TOOLCHAIN_METADATA_DIR}/Zahlen${name}ConfigVersion.cmake
        DESTINATION ${dest} COMPONENT Zahlen${name})
endfunction()

# Binary shared-library package (used SOLELY for ZahlenEngine -> libzahlen_engine.so).
function(zhln_toolchain_binary_package name)
    cmake_parse_arguments(P "" "" "TARGETS;DEPENDS;HEADERS" ${ARGN})
    list(GET P_TARGETS 0 first_target)
    if(NOT TARGET ${first_target})
        return()
    endif()
    foreach(target IN LISTS P_TARGETS)
        zhln_toolchain_interface(${target})
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
        install(TARGETS ${target} EXPORT Zahlen${name}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Zahlen${name}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Zahlen${name})
    endforeach()
    foreach(dir IN LISTS P_HEADERS)
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/${dir}/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT Zahlen${name}
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "*.inl")
    endforeach()
    _zhln_write_package_config("${name}" "${P_DEPENDS}")
    install(EXPORT Zahlen${name}Targets NAMESPACE Zahlen::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Zahlen${name} COMPONENT Zahlen${name})
    string(TOLOWER "${name}" _pkg_leaf)
    set(_provides "")
    foreach(target IN LISTS P_TARGETS)
        get_target_property(_export_name ${target} EXPORT_NAME)
        if(NOT _export_name)
            set(_export_name ${target})
        endif()
        list(APPEND _provides "Zahlen::${_export_name}")
    endforeach()
    zhln_toolchain_manifest("zahlen-${_pkg_leaf}" "${name}" PROVIDES ${_provides})
endfunction()

# Source/header package for optional COM extensions and plugins. Ships ZERO .a
# archives: consumer toolchains compile the installed sources directly against
# Zahlen::Engine using their own compiler version, flags, and build type.
function(zhln_toolchain_source_package name)
    cmake_parse_arguments(P "ENABLE_C;NO_REFLECTION" "CHECK_TARGET;EXPORT_NAME;ALIAS_NAME"
        "DEPENDS;HEADER_DIRS;HEADER_FILES;HEADER_DEST;SOURCES;MODULES;PUBLIC_LINKS;PRIVATE_LINKS;PRIVATE_DEFS;VENDORED_HEADERS" ${ARGN})
    if(P_CHECK_TARGET AND NOT TARGET ${P_CHECK_TARGET})
        return()
    endif()
    if(NOT P_EXPORT_NAME)
        set(P_EXPORT_NAME "${name}")
    endif()
    string(TOLOWER "${name}" _pkg_leaf)
    set(_src_rel "${CMAKE_INSTALL_DATADIR}/zahlen/src/${_pkg_leaf}")

    # Install public headers (and track leaf subdirs so sibling #include "Foo.hpp" resolves)
    set(_priv_inc_subdirs "")
    foreach(dir IN LISTS P_HEADER_DIRS)
        set(_hdest "${CMAKE_INSTALL_INCLUDEDIR}")
        if(NOT dir STREQUAL "include" AND NOT dir STREQUAL "app/include")
            get_filename_component(_leaf "${dir}" NAME)
            string(APPEND _hdest "/${_leaf}")
            list(APPEND _priv_inc_subdirs "${_leaf}")
        endif()
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/${dir}/"
            DESTINATION "${_hdest}" COMPONENT Zahlen${name}
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "*.inl")
        # The .in is a template, not a header: a consumer that installed it as a
        # header would read a version that was never configured.
        install(CODE "file(REMOVE \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${_hdest}/Zahlen/Version.hpp.in\")"
            COMPONENT Zahlen${name})
    endforeach()
    if(P_HEADER_FILES)
        set(_fdest "${CMAKE_INSTALL_INCLUDEDIR}")
        if(P_HEADER_DEST)
            string(APPEND _fdest "/${P_HEADER_DEST}")
            list(APPEND _priv_inc_subdirs "${P_HEADER_DEST}")
        endif()
        set(_abs_hfiles "")
        foreach(hf IN LISTS P_HEADER_FILES)
            list(APPEND _abs_hfiles "${PROJECT_SOURCE_DIR}/${hf}")
        endforeach()
        install(FILES ${_abs_hfiles} DESTINATION "${_fdest}" COMPONENT Zahlen${name})
    endif()

    # Install optional private vendored single-header dependencies (e.g. stb, cgltf)
    set(_has_vendored FALSE)
    if(P_VENDORED_HEADERS)
        set(_has_vendored TRUE)
        set(_abs_vfiles "")
        foreach(vf IN LISTS P_VENDORED_HEADERS)
            list(APPEND _abs_vfiles "${PROJECT_SOURCE_DIR}/${vf}")
        endforeach()
        install(FILES ${_abs_vfiles} DESTINATION "${_src_rel}/vendor" COMPONENT Zahlen${name})
    endif()

    # Install implementation sources & C++20/26 module units
    set(_rel_sources "")
    foreach(sf IN LISTS P_SOURCES)
        get_filename_component(_sname "${sf}" NAME)
        install(FILES "${PROJECT_SOURCE_DIR}/${sf}" DESTINATION "${_src_rel}" COMPONENT Zahlen${name})
        list(APPEND _rel_sources "\${_zhln_src_dir}/${_sname}")
    endforeach()
    set(_rel_modules "")
    foreach(mf IN LISTS P_MODULES)
        get_filename_component(_mname "${mf}" NAME)
        install(FILES "${PROJECT_SOURCE_DIR}/${mf}" DESTINATION "${_src_rel}" COMPONENT Zahlen${name})
        list(APPEND _rel_modules "\${_zhln_src_dir}/${_mname}")
    endforeach()

    # Generate relocatable Zahlen<Name>Targets.cmake
    set(_tname "zahlen_pkg_${_pkg_leaf}")
    set(_script "# Generated source-package target for Zahlen::${P_EXPORT_NAME} (no prebuilt .a)\n")
    string(APPEND _script "if(NOT TARGET Zahlen::${P_EXPORT_NAME})\n")
    string(APPEND _script "  get_filename_component(_zhln_pkg_prefix \"\${CMAKE_CURRENT_LIST_DIR}/../../..\" ABSOLUTE)\n")
    string(APPEND _script "  set(_zhln_inc_dir \"\${_zhln_pkg_prefix}/${CMAKE_INSTALL_INCLUDEDIR}\")\n")
    if(P_ENABLE_C)
        string(APPEND _script "  enable_language(C)\n")
    endif()
    if(_rel_sources OR _rel_modules)
        string(APPEND _script "  set(_zhln_src_dir \"\${_zhln_pkg_prefix}/${_src_rel}\")\n")
        string(APPEND _script "  add_library(${_tname} STATIC EXCLUDE_FROM_ALL ${_rel_sources})\n")
        string(APPEND _script "  set_target_properties(${_tname} PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)\n")
        if(P_ENABLE_C)
            string(APPEND _script "  set_target_properties(${_tname} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)\n")
        endif()
        if(_rel_modules)
            string(APPEND _script "  target_sources(${_tname} PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES BASE_DIRS \"\${_zhln_src_dir}\" FILES ${_rel_modules})\n")
        endif()
        string(APPEND _script "  target_include_directories(${_tname} SYSTEM PUBLIC \"\${_zhln_inc_dir}\")\n")
        list(REMOVE_DUPLICATES _priv_inc_subdirs)
        foreach(_sub IN LISTS _priv_inc_subdirs)
            string(APPEND _script "  target_include_directories(${_tname} PRIVATE \"\${_zhln_inc_dir}/${_sub}\")\n")
        endforeach()
        if(_has_vendored)
            string(APPEND _script "  target_include_directories(${_tname} SYSTEM PRIVATE \"\${_zhln_src_dir}/vendor\")\n")
            string(APPEND _script "  target_compile_options(${_tname} PRIVATE \"--embed-dir=\${_zhln_src_dir}/vendor\")\n")
        endif()
        string(APPEND _script "  target_compile_features(${_tname} PUBLIC cxx_std_26)\n")
        string(APPEND _script "  target_compile_options(${_tname} PUBLIC \"\$<\$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>\")\n")
        if(NOT P_NO_REFLECTION AND NOT ZHLN_TOOLCHAIN_REFLECTION_FLAGS STREQUAL "")
            string(APPEND _script "  target_compile_options(${_tname} PUBLIC \"\$<\$<COMPILE_LANGUAGE:CXX>:${ZHLN_TOOLCHAIN_REFLECTION_FLAGS}>\")\n")
        endif()
        if(P_PUBLIC_LINKS)
            string(APPEND _script "  target_link_libraries(${_tname} PUBLIC ${P_PUBLIC_LINKS})\n")
        endif()
        if(P_PRIVATE_LINKS)
            string(APPEND _script "  target_link_libraries(${_tname} PRIVATE ${P_PRIVATE_LINKS})\n")
        endif()
        if(P_PRIVATE_DEFS)
            string(APPEND _script "  target_compile_definitions(${_tname} PRIVATE ${P_PRIVATE_DEFS})\n")
        endif()
        string(APPEND _script "  add_library(Zahlen::${P_EXPORT_NAME} ALIAS ${_tname})\n")
        if(P_ALIAS_NAME)
            string(APPEND _script "  add_library(Zahlen::${P_ALIAS_NAME} ALIAS ${_tname})\n")
        endif()
        string(APPEND _script "  unset(_zhln_src_dir)\n")
    else()
        # Pure header-only package
        string(APPEND _script "  add_library(${_tname} INTERFACE)\n")
        string(APPEND _script "  target_include_directories(${_tname} SYSTEM INTERFACE \"\${_zhln_inc_dir}\")\n")
        string(APPEND _script "  target_compile_features(${_tname} INTERFACE cxx_std_26)\n")
        if(NOT P_NO_REFLECTION AND NOT ZHLN_TOOLCHAIN_REFLECTION_FLAGS STREQUAL "")
            string(APPEND _script "  target_compile_options(${_tname} INTERFACE \"\$<\$<COMPILE_LANGUAGE:CXX>:${ZHLN_TOOLCHAIN_REFLECTION_FLAGS}>\")\n")
        endif()
        if(P_PUBLIC_LINKS)
            string(APPEND _script "  target_link_libraries(${_tname} INTERFACE ${P_PUBLIC_LINKS})\n")
        endif()
        string(APPEND _script "  add_library(Zahlen::${P_EXPORT_NAME} ALIAS ${_tname})\n")
        if(P_ALIAS_NAME)
            string(APPEND _script "  add_library(Zahlen::${P_ALIAS_NAME} ALIAS ${_tname})\n")
        endif()
    endif()
    string(APPEND _script "  unset(_zhln_inc_dir)\n  unset(_zhln_pkg_prefix)\nendif()\n")

    set(_targets_file "${ZHLN_TOOLCHAIN_METADATA_DIR}/Zahlen${name}Targets.cmake")
    file(WRITE "${_targets_file}" "${_script}")
    _zhln_write_package_config("${name}" "${P_DEPENDS}")
    install(FILES "${_targets_file}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Zahlen${name}" COMPONENT Zahlen${name})

    set(_requires "")
    foreach(dep IN LISTS P_DEPENDS)
        string(TOLOWER "${dep}" _dep_leaf)
        list(APPEND _requires "zahlen-${_dep_leaf}")
    endforeach()
    set(_provides "Zahlen::${P_EXPORT_NAME}")
    if(P_ALIAS_NAME)
        list(APPEND _provides "Zahlen::${P_ALIAS_NAME}")
    endif()
    zhln_toolchain_manifest("zahlen-${_pkg_leaf}" "${name}"
        REQUIRES ${_requires} PROVIDES ${_provides})
endfunction()

# Install a dynamically loaded COM runtime plugin (.so/.dylib/.dll) and its
# companion .manifest.toml into the relocatable plugin directory. The plugin
# resolves libzahlen_engine.so via INSTALL_RPATH "$ORIGIN/../.." without
# requiring LD_LIBRARY_PATH, and is never exported as a link-time CMake target.
function(zhln_toolchain_install_plugin target component manifest_relpath)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN/../..")
        install(TARGETS ${target}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/zahlen/plugins COMPONENT ${component}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT ${component})
        if(EXISTS "${PROJECT_SOURCE_DIR}/${manifest_relpath}")
            install(FILES "${PROJECT_SOURCE_DIR}/${manifest_relpath}"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/zahlen/plugins COMPONENT ${component})
        endif()
    endif()
endfunction()

# --- 1. Core shared-library package: libzahlen_engine.so ONLY ---------------
set_target_properties(zahlen_engine PROPERTIES EXPORT_NAME Engine)
if(ZHLN_DEV_MODE AND ZHLN_DEV_MODE_IN_INSTALLED_PACKAGES)
    target_compile_definitions(zahlen_engine INTERFACE ZHLN_DEV_MODE)
endif()
if(ZHLN_NO_ANNOTATION_EXTRACT)
    target_compile_definitions(zahlen_engine INTERFACE ZHLN_NO_ANNOTATION_EXTRACT)
endif()
zhln_toolchain_binary_package(Engine TARGETS zahlen_engine HEADERS include)
# <Zahlen/Version.hpp> is generated, so it is not in include/ for the rule above
# to pick up. It ships with the headers that include it, in every package that
# can see <Zahlen/Config.hpp>.
install(FILES "${ZHLN_GENERATED_INCLUDE_DIR}/Zahlen/Version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Zahlen COMPONENT ZahlenEngine)
# --- 2. Host shell & standalone utilities (source/header packages) ----------
zhln_toolchain_source_package(CommandLine
    CHECK_TARGET zahlen_command_line
    HEADER_DIRS extensions/CommandLine
    SOURCES extensions/CommandLine/CommandLine.cpp
    PUBLIC_LINKS Zahlen::Engine
    DEPENDS Engine)

zhln_toolchain_source_package(Image
    CHECK_TARGET zahlen_image
    NO_REFLECTION
    HEADER_DIRS extensions/Image
    SOURCES extensions/Image/Image.cpp
    VENDORED_HEADERS extern/stb/stb_image.h extern/stb/stb_image_write.h)

zhln_toolchain_source_package(Signal
    CHECK_TARGET zahlen_signal
    DEPENDS Engine
    HEADER_DIRS extensions/Signal
    SOURCES extensions/Signal/SignalManager.cpp
            extensions/Signal/StackSymbolizer.cpp
            extensions/Signal/WorkerCrashReports.cpp
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(Screenshot
    CHECK_TARGET zahlen_screenshot
    DEPENDS Engine Image
    HEADER_DIRS extensions/Screenshot
    SOURCES extensions/Screenshot/Screenshot.cpp
    PUBLIC_LINKS Zahlen::Engine
    PRIVATE_LINKS Zahlen::Image)
zhln_toolchain_install_plugin(zahlen_plugin_screenshot ZahlenScreenshot
    extensions/Screenshot/zahlen_plugin_screenshot.manifest.toml)

zhln_toolchain_source_package(HostRunner
    CHECK_TARGET zahlen_host_runner
    DEPENDS Engine CommandLine Signal
    HEADER_FILES app/include/Zahlen/HostRunner.hpp app/include/Zahlen/AppOutcome.hpp
    HEADER_DEST Zahlen
    SOURCES app/HostRunner.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::CommandLine
    PRIVATE_LINKS Zahlen::Signal)

zhln_toolchain_source_package(Application
    CHECK_TARGET zahlen_application
    DEPENDS Engine CommandLine HostRunner Screenshot
    HEADER_FILES app/include/Zahlen/App.hpp app/include/Zahlen/ConfigCli.hpp app/include/Zahlen/Zahlen.hpp
                 app/include/Zahlen/PluginManifest.hpp
                 app/include/Zahlen/PluginManifestGraph.hpp
                 app/include/Zahlen/PluginManifestRegistry.hpp
    HEADER_DEST Zahlen
    SOURCES app/Application.cpp
            app/PluginManifest.cpp
            app/PluginManifestGraph.cpp
            app/PluginManifestRegistry.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::CommandLine Zahlen::HostRunner
    PRIVATE_LINKS Zahlen::Screenshot)

# --- 3. Plugins, UI & serialization (source/header packages) ----------------
zhln_toolchain_source_package(Netpbm
    CHECK_TARGET zahlen_netpbm
    DEPENDS Engine
    HEADER_DIRS plugins/netpbm
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(UI
    CHECK_TARGET zahlen_ui
    DEPENDS Engine
    HEADER_FILES extensions/UI/FontLoader.hpp
                 extensions/UI/GUI.hpp
                 extensions/UI/HostUI.hpp
                 extensions/UI/Text.hpp
                 extensions/UI/TextBuffer.hpp
    HEADER_DEST UI
    SOURCES extensions/UI/FontLoader.cpp
            extensions/UI/GUIContext.cpp
            extensions/UI/HostUI.cpp
            extensions/UI/Text.cpp
    VENDORED_HEADERS extern/clay/clay.h resources/fonts/DefaultFont.zfont
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(UISchema
    CHECK_TARGET zahlen_ui_schema
    DEPENDS Engine UI
    HEADER_FILES extensions/UI/UITree.hpp
    HEADER_DEST UI
    SOURCES extensions/UI/UITree.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::UI)

zhln_toolchain_source_package(Scene
    CHECK_TARGET zahlen_scene
    DEPENDS Engine
    HEADER_DIRS plugins/Scene
    SOURCES plugins/Scene/Scene.cpp
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(Serialization
    CHECK_TARGET zahlen_serialization
    DEPENDS Engine UISchema Scene
    HEADER_DIRS plugins/json plugins/toml
    SOURCES plugins/json/JSON.cpp plugins/toml/TOML.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::Scene
    PRIVATE_LINKS simdjson::simdjson)

zhln_toolchain_source_package(GLTF
    CHECK_TARGET zahlen_gltf_assets
    ENABLE_C
    EXPORT_NAME GLTF
    ALIAS_NAME GLTFAssets
    DEPENDS Engine Serialization Image
    HEADER_DIRS plugins/glTF
    SOURCES plugins/glTF/GLTFDocument.cpp
            plugins/glTF/GLTFAssets.cpp
            plugins/glTF/TangentGenerator.cpp
            plugins/glTF/cgltf_impl.c
    VENDORED_HEADERS extern/cgltf/cgltf.h
    PUBLIC_LINKS Zahlen::Engine
    PRIVATE_LINKS Zahlen::Serialization Zahlen::Image)
zhln_toolchain_install_plugin(zahlen_plugin_gltf ZahlenGLTF
    plugins/glTF/zahlen_plugin_gltf.manifest.toml)

zhln_toolchain_source_package(Fonts
    CHECK_TARGET zahlen_fonts
    DEPENDS Engine UI Serialization Image
    HEADER_DIRS plugins/Fonts
    SOURCES plugins/Fonts/Fonts.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::UI
    PRIVATE_LINKS Zahlen::Image Zahlen::Serialization)
if(TARGET zahlen_fonts)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/resources/fonts/JetBrainsMonoNerdFontRegular/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen/fonts/JetBrainsMonoNerdFontRegular
        COMPONENT ZahlenFonts)
endif()

zhln_toolchain_source_package(Lua
    CHECK_TARGET zahlen_lua
    DEPENDS Engine Scene
    HEADER_DIRS plugins/lua
    SOURCES plugins/lua/Lua.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::Scene
    PRIVATE_LINKS LuaJIT)

# --- 4. Optional COM extensions (source/header packages) --------------------
zhln_toolchain_source_package(Widgets
    CHECK_TARGET zahlen_widgets
    DEPENDS Engine UI
    HEADER_DIRS extensions/Widget
    SOURCES extensions/Widget/BarStrip.cpp extensions/Widget/HoverCard.cpp extensions/Widget/TextFit.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::UI)

zhln_toolchain_source_package(Benchmark
    CHECK_TARGET zahlen_benchmark
    DEPENDS Engine
    HEADER_FILES extensions/Benchmark/Benchmark.hpp
    HEADER_DEST Benchmark
    SOURCES extensions/Benchmark/Benchmark.cpp
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(BenchmarkViewer
    CHECK_TARGET zahlen_benchmark_viewer
    DEPENDS Engine UI Benchmark Widgets
    HEADER_FILES extensions/Benchmark/Viewer/BenchmarkViewer.hpp
                 extensions/Benchmark/Viewer/FrameTelemetry.hpp
                 extensions/Benchmark/Viewer/SystemProbe.hpp
                 extensions/Benchmark/Viewer/SystemTelemetry.hpp
    HEADER_DEST Benchmark/Viewer
    SOURCES extensions/Benchmark/Viewer/BenchmarkViewer.cpp
            extensions/Benchmark/Viewer/FrameTelemetry.cpp
            extensions/Benchmark/Viewer/SystemTelemetry.cpp
            extensions/Benchmark/Viewer/SystemProbeLinux.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::UI Zahlen::Benchmark Zahlen::Widgets)

zhln_toolchain_source_package(Scripting
    CHECK_TARGET zahlen_scripting
    DEPENDS Engine
    HEADER_DIRS extensions/Scripting
    SOURCES extensions/Scripting/ScriptECSBridge.cpp
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(Console
    CHECK_TARGET zahlen_console
    DEPENDS Engine Scripting
    HEADER_DIRS extensions/Console
    SOURCES extensions/Console/Console.cpp extensions/Console/ConsoleDebugger.cpp extensions/Console/SymbolResolver.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::Scripting)

zhln_toolchain_source_package(Editor
    CHECK_TARGET zahlen_editor
    DEPENDS Engine UI
    HEADER_FILES extensions/editor/GUIEditor.hpp
    HEADER_DEST editor
    SOURCES extensions/editor/GUIEditor.cpp
    PUBLIC_LINKS Zahlen::Engine Zahlen::UI)

zhln_toolchain_source_package(Terrain
    CHECK_TARGET zahlen_terrain
    DEPENDS Engine
    HEADER_FILES extensions/Terrain/Terrain.hpp
                 extensions/Terrain/TerrainGeometry.hpp
                 extensions/Terrain/TerrainComponents.hpp
                 extensions/Terrain/TerrainTypes.hpp
                 extensions/Terrain/ITerrainLayer.hpp
    HEADER_DEST Terrain
    SOURCES extensions/Terrain/TerrainGeometry.cpp extensions/Terrain/Terrain.cpp
    PUBLIC_LINKS Zahlen::Engine)
zhln_toolchain_install_plugin(zahlen_plugin_terrain ZahlenTerrain
    extensions/Terrain/zahlen_plugin_terrain.manifest.toml)

zhln_toolchain_source_package(VFX
    CHECK_TARGET zahlen_vfx
    DEPENDS Engine
    HEADER_FILES extensions/VFX/CombatFX.hpp
                 extensions/VFX/Lightning.hpp
                 extensions/VFX/LightningGeometry.hpp
                 extensions/VFX/Explosion.hpp
                 extensions/VFX/ExplosionPolicy.hpp
                 extensions/VFX/VFXTypes.hpp
                 extensions/VFX/IVfxLayer.hpp
    HEADER_DEST VFX
    SOURCES extensions/VFX/CombatFXLifecycle.cpp
            extensions/VFX/LightningGeometry.cpp
            extensions/VFX/Lightning.cpp
            extensions/VFX/Explosion.cpp
    PUBLIC_LINKS Zahlen::Engine)
zhln_toolchain_install_plugin(zahlen_plugin_vfx ZahlenVFX
    extensions/VFX/zahlen_plugin_vfx.manifest.toml)

zhln_toolchain_source_package(FreeCam
    CHECK_TARGET zahlen_freecam
    DEPENDS Engine
    HEADER_DIRS extensions/FreeCam
    SOURCES extensions/FreeCam/FreeCam.cpp
    PUBLIC_LINKS Zahlen::Engine)
zhln_toolchain_install_plugin(zahlen_plugin_freecam ZahlenFreeCam
    extensions/FreeCam/zahlen_plugin_freecam.manifest.toml)

zhln_toolchain_source_package(Camera
    CHECK_TARGET zahlen_camera
    DEPENDS Engine
    HEADER_DIRS extensions/Camera
    SOURCES extensions/Camera/TargetCamera.cpp
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(CharacterController
    CHECK_TARGET zahlen_character_controller
    DEPENDS Engine
    HEADER_DIRS extensions/CharacterController
    SOURCES extensions/CharacterController/CharacterController.cpp
    PUBLIC_LINKS Zahlen::Engine)
zhln_toolchain_install_plugin(zahlen_plugin_character_controller ZahlenCharacterController
    extensions/CharacterController/zahlen_plugin_character_controller.manifest.toml)

zhln_toolchain_source_package(AnimationControls
    CHECK_TARGET zahlen_animation_controls
    DEPENDS Engine
    HEADER_FILES extensions/Animation/Animator.hpp
                 extensions/Animation/AnimationID.hpp
                 extensions/Animation/Components.hpp
                 extensions/Animation/IK.hpp
                 extensions/Animation/IKControls.hpp
    HEADER_DEST Animation
    SOURCES extensions/Animation/Animator.cpp
    MODULES extensions/Animation/Animator.cppm
    PUBLIC_LINKS Zahlen::Engine)

zhln_toolchain_source_package(Animation
    CHECK_TARGET zahlen_animation
    DEPENDS Engine AnimationControls CharacterController Camera FreeCam
    HEADER_DIRS extensions/Animation
    SOURCES extensions/Animation/TwoBoneIK.cpp
            extensions/Animation/Context.cpp
            extensions/Animation/AnimationSystem.cpp
            extensions/Animation/LocomotionLifecycle.cpp
            extensions/Animation/ProceduralAnimation.cpp
            extensions/Animation/ProceduralGait.cpp
            extensions/Animation/ProceduralItemHandling.cpp
            extensions/Animation/ProceduralSecondaryPhysics.cpp
    MODULES extensions/Animation/Rig.cppm
            extensions/Animation/Locomotion.cppm
            extensions/Animation/ProceduralAnimation.cppm
    PUBLIC_LINKS Zahlen::Engine
                 Zahlen::AnimationControls
                 Zahlen::CharacterController
                 Zahlen::Camera
                 Zahlen::FreeCam)
zhln_toolchain_install_plugin(zahlen_plugin_animation ZahlenAnimation
    extensions/Animation/zahlen_plugin_animation.manifest.toml)

zhln_toolchain_source_package(AssetCooking
    CHECK_TARGET zahlen_asset_cooking
    DEPENDS Engine Image
    HEADER_DIRS plugins/AssetCooking
    SOURCES plugins/AssetCooking/EnvironmentPreparation.cpp
            plugins/AssetCooking/RadianceDecoder.cpp
            plugins/AssetCooking/RadianceEncoder.cpp
            plugins/AssetCooking/MeshCook.cpp
            plugins/AssetCooking/AnimCook.cpp
            plugins/AssetCooking/PakWriter.cpp
            plugins/AssetCooking/FontBake.cpp
    VENDORED_HEADERS extern/stb/stb_truetype.h
    PUBLIC_LINKS Zahlen::Engine
    PRIVATE_LINKS Zahlen::Image)

# --- 5. Offline cooker tool -------------------------------------------------
if(TARGET zcook)
    set_target_properties(zcook PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS zcook RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ZahlenCooker)
    install(FILES ${PROJECT_SOURCE_DIR}/scripts/export_metadata.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen/tools COMPONENT ZahlenCooker)
    zhln_toolchain_manifest(zahlen-cooker Cooker
        REQUIRES zahlen-engine PROVIDES zcook)
    # find_package(ZahlenCooker) gives consumers Zahlen::zcook, an IMPORTED
    # executable they can drive from their own asset graph.
    configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/toolchain/CookerConfig.cmake.in
        ${ZHLN_TOOLCHAIN_METADATA_DIR}/ZahlenCookerConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZahlenCooker)
    write_basic_package_version_file(${ZHLN_TOOLCHAIN_METADATA_DIR}/ZahlenCookerConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES
        ${ZHLN_TOOLCHAIN_METADATA_DIR}/ZahlenCookerConfig.cmake
        ${ZHLN_TOOLCHAIN_METADATA_DIR}/ZahlenCookerConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZahlenCooker COMPONENT ZahlenCooker)
endif()
