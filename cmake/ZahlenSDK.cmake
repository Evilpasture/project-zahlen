# Installed interfaces, independent of the OS package manager.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The initial SDK packaging profile supports Linux only")
endif()

# The existing engine is shared; optional packages are static. Native exports
# preserve their PRIVATE dependencies as LINK_ONLY, including Serialization.
if(ZHLN_USE_CUSTOM_LIBCXX)
    message(FATAL_ERROR "SDK packaging currently requires the system libstdc++ toolchain")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "SDK packaging requires a single-config Release build")
endif()
if(NOT ZHLN_HAS_SIMDJSON AND TARGET zahlen_serialization)
    message(FATAL_ERROR "SDK packaging requires a system simdjson CONFIG package")
endif()
if(TARGET zahlen_serialization)
    find_package(simdjson CONFIG REQUIRED)
endif()
set(ZHLN_SDK_PROFILE release)
if(USE_SANITIZERS)
    set(ZHLN_SDK_PROFILE asan-ubsan)
endif()
# Capture the standard-library ABI selected by this toolchain, not merely its
# compiler brand (Clang can use either libc++ or libstdc++).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdk-stl.cpp" "#include <string>\n")
separate_arguments(_sdk_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${_sdk_flags} -dM -E -x c++
    "${CMAKE_CURRENT_BINARY_DIR}/sdk-stl.cpp"
    OUTPUT_VARIABLE _sdk_macros COMMAND_ERROR_IS_FATAL ANY)
foreach(macro __GLIBCXX__ _GLIBCXX_USE_CXX11_ABI)
    if(NOT _sdk_macros MATCHES "#define ${macro} ([0-9]+)")
        message(FATAL_ERROR "SDK packaging requires libstdc++ (${macro} missing)")
    endif()
    set(ZHLN_SDK_${macro} "${CMAKE_MATCH_1}")
endforeach()
if(_sdk_macros MATCHES "#define _GLIBCXX_DEBUG[ \t\r\n]")
    message(FATAL_ERROR "SDK packaging does not support libstdc++ debug containers")
endif()
get_target_property(_sdk_jolt_defs Jolt INTERFACE_COMPILE_DEFINITIONS)
get_target_property(_sdk_jolt_options Jolt INTERFACE_COMPILE_OPTIONS)
# Commit identity intentionally makes SDK upgrades atomic, even before stable ABI.
set(ZHLN_SDK_ABI "${PROJECT_VERSION}-${ZHLN_GIT_HASH}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_SYSTEM_PROCESSOR}-${ZHLN_SDK_PROFILE}")

string(SHA256 ZHLN_SDK_ABI "${ZHLN_SDK_ABI};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE};${_sdk_jolt_defs};${_sdk_jolt_options};${ZHLN_NO_ANNOTATION_EXTRACT};${ZHLN_DEV_MODE};${ZHLN_BUILD_RENDER};${ZHLN_SDK___GLIBCXX__};${ZHLN_SDK__GLIBCXX_USE_CXX11_ABI}")

# Make existing include roots relocatable without changing in-tree behavior.
function(zhln_sdk_interface target)
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

# One export set and install component per package. Dependencies are explicit;
# CMake itself checks that every linked non-imported target is exported.
function(zhln_sdk_package name)
    cmake_parse_arguments(P "" "" "TARGETS;DEPENDS;HEADERS" ${ARGN})
    list(GET P_TARGETS 0 first_target)
    if(NOT TARGET ${first_target})
        return()
    endif()
    set(ZHLN_SDK_DEPENDENCIES "")
    foreach(dep IN LISTS P_DEPENDS)
        string(APPEND ZHLN_SDK_DEPENDENCIES "find_dependency(Zahlen${dep} ${PROJECT_VERSION} EXACT CONFIG)\n")
    endforeach()
    if(name STREQUAL "Serialization")
        string(APPEND ZHLN_SDK_DEPENDENCIES "find_dependency(simdjson CONFIG)\n")
    endif()
    foreach(target IN LISTS P_TARGETS)
        zhln_sdk_interface(${target})
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
    set(ZHLN_SDK_PACKAGE ${name})
    configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/sdk/PackageConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/sdk/Zahlen${name}Config.cmake
        INSTALL_DESTINATION ${dest})
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/sdk/Zahlen${name}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sdk/Zahlen${name}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/sdk/Zahlen${name}ConfigVersion.cmake
        DESTINATION ${dest} COMPONENT Zahlen${name})
    install(EXPORT Zahlen${name}Targets NAMESPACE Zahlen::
        DESTINATION ${dest} COMPONENT Zahlen${name}
        CXX_MODULES_DIRECTORY cxx-modules)
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
zhln_sdk_package(Engine TARGETS zahlen_engine Jolt HEADERS include)
# Jolt's headers must retain their Jolt/ prefix.
install(DIRECTORY extern/JoltPhysics/Jolt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT ZahlenEngine FILES_MATCHING PATTERN "*.h" PATTERN "*.inl")
zhln_sdk_package(CommandLine TARGETS zahlen_command_line HEADERS extensions/CommandLine)
zhln_sdk_package(Signal TARGETS zahlen_signal DEPENDS Engine HEADERS extensions/Signal)
zhln_sdk_package(HostRunner TARGETS zahlen_host_runner DEPENDS Engine CommandLine Signal)
zhln_sdk_package(Application TARGETS zahlen_application DEPENDS Engine CommandLine HostRunner)
install(FILES app/include/Zahlen/HostRunner.hpp app/include/Zahlen/KernelApp.hpp
    app/include/Zahlen/AppOutcome.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Zahlen
    COMPONENT ZahlenHostRunner)
install(FILES app/include/Zahlen/App.hpp app/include/Zahlen/ConfigCli.hpp
    app/include/Zahlen/Zahlen.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Zahlen
    COMPONENT ZahlenApplication)
zhln_sdk_package(Netpbm TARGETS zahlen_netpbm DEPENDS Engine HEADERS plugins/netpbm)
zhln_sdk_package(Screenshot TARGETS zahlen_screenshot DEPENDS Engine HEADERS extensions/Screenshot)
zhln_sdk_package(UISchema TARGETS zahlen_ui_schema DEPENDS Engine HEADERS extensions/UI)
zhln_sdk_package(Serialization TARGETS zahlen_serialization DEPENDS Engine UISchema HEADERS plugins/json plugins/toml)
zhln_sdk_package(GLTF TARGETS zahlen_gltf DEPENDS Engine Serialization HEADERS plugins/glTF)
zhln_sdk_package(FreeCam TARGETS zahlen_freecam DEPENDS Engine HEADERS extensions/FreeCam)
zhln_sdk_package(Camera TARGETS zahlen_camera DEPENDS Engine FreeCam HEADERS extensions/Camera)
zhln_sdk_package(CharacterController TARGETS zahlen_character_controller DEPENDS Engine FreeCam HEADERS extensions/CharacterController)
zhln_sdk_package(SkeletalAnimation TARGETS zahlen_skeletal_animation DEPENDS Engine HEADERS extensions/Animation)
zhln_sdk_package(Animation TARGETS zahlen_animation DEPENDS Engine SkeletalAnimation CharacterController Camera FreeCam)

# Data is separately installable. Consumers stage it beside their executable or
# point ZHLN_DATA_DIR at this directory. No compiled-in source-tree path needed.
# AssetPipeline stages base.pak as a symlink to its shared build cache. Installing
# that link would leave the SDK dependent on the engine checkout/cache. Copy the
# contents at INSTALL time (the cooker has not necessarily run at configure time).
install(CODE "
    file(COPY_FILE \"${CMAKE_BINARY_DIR}/data/base.pak\"
        \"${CMAKE_BINARY_DIR}/sdk/base.pak\" ONLY_IF_DIFFERENT)
" COMPONENT ZahlenData)
install(FILES ${CMAKE_BINARY_DIR}/sdk/base.pak
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen/data COMPONENT ZahlenData)
install(DIRECTORY resources DESTINATION ${CMAKE_INSTALL_DATADIR}/zahlen COMPONENT ZahlenData)
