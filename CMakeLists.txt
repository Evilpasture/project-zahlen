cmake_minimum_required(VERSION 3.25)

# Force the compiler selection BEFORE project()
if(WIN32 AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(zahlen LANGUAGES C CXX ASM)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable LLGL extras
set(LLGL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLGL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LLGL SYSTEM)

# --- LuaJIT Build (Fixed for Windows Semicolon Bug) ---
include(ExternalProject)
set(LUAJIT_ROOT_DIR "${CMAKE_SOURCE_DIR}/extern/LuaJIT")
set(LUAJIT_SRC_DIR "${LUAJIT_ROOT_DIR}/src")

if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT (MSVC OR DEFINED ENV{INCLUDE} OR DEFINED ENV{VSINSTALLDIR} OR DEFINED ENV{VCINSTALLDIR}))
  message(FATAL_ERROR "Windows clang/lld builds require LuaJIT to be built with MSVC tools. Open a Visual Studio Developer Prompt or set INCLUDE/VSINSTALLDIR/VCINSTALLDIR before configuring.")
endif()

if(MSVC OR DEFINED ENV{INCLUDE} OR DEFINED ENV{VSINSTALLDIR} OR DEFINED ENV{VCINSTALLDIR})
  ExternalProject_Add(LuaJITProject
      SOURCE_DIR "${LUAJIT_ROOT_DIR}"
      BUILD_IN_SOURCE 1
      CONFIGURE_COMMAND ""
      # We run the command from the ROOT, and the command itself handles the 'cd' to 'src'
      # If the Visual Studio build environment is not loaded, fail early.
      BUILD_COMMAND cmd /c "if not defined INCLUDE (echo ERROR: Visual Studio environment not loaded && exit /b 1) && cd src && msvcbuild.bat static && if exist lua51.lib (exit /b 0) else (echo ERROR: LuaJIT build failed - lua51.lib not found && exit /b 1)"
      INSTALL_COMMAND ""
      # Explicitly state where the byproduct will land
      BUILD_BYPRODUCTS "${LUAJIT_SRC_DIR}/lua51.lib"
      # Prevent directory hopping
      UPDATE_COMMAND ""
  )
  set(LUAJIT_LIB_FILE "${LUAJIT_SRC_DIR}/lua51.lib")
elseif(WIN32)
  message(WARNING "Building LuaJIT on Windows without a Visual Studio environment may produce a MinGW library incompatible with clang/lld. Use a VS Developer Prompt or set INCLUDE/VSINSTALLDIR/VCINSTALLDIR before configuring.")
  find_program(LUAJIT_MAKE NAMES gmake make REQUIRED)
  ExternalProject_Add(LuaJITProject
      SOURCE_DIR "${LUAJIT_ROOT_DIR}"
      BINARY_DIR "${LUAJIT_ROOT_DIR}"
      CONFIGURE_COMMAND ""
      BUILD_COMMAND ${LUAJIT_MAKE} -C src
      INSTALL_COMMAND ""
      BUILD_BYPRODUCTS "${LUAJIT_SRC_DIR}/libluajit.a"
  )
  set(LUAJIT_LIB_FILE "${LUAJIT_SRC_DIR}/libluajit.a")
else()
  find_program(LUAJIT_MAKE NAMES gmake make REQUIRED)
  ExternalProject_Add(LuaJITProject
      SOURCE_DIR "${LUAJIT_ROOT_DIR}"
      BINARY_DIR "${LUAJIT_ROOT_DIR}"
      CONFIGURE_COMMAND ""
      BUILD_COMMAND ${LUAJIT_MAKE}
      INSTALL_COMMAND ""
      BUILD_BYPRODUCTS "${LUAJIT_SRC_DIR}/libluajit.a"
  )
  set(LUAJIT_LIB_FILE "${LUAJIT_SRC_DIR}/libluajit.a")
endif()

add_library(LuaJIT STATIC IMPORTED GLOBAL)
set_target_properties(LuaJIT PROPERTIES
    IMPORTED_LOCATION "${LUAJIT_LIB_FILE}"
    INTERFACE_INCLUDE_DIRECTORIES "${LUAJIT_SRC_DIR}"
)
add_dependencies(LuaJIT LuaJITProject)


# --- Jolt Physics ---
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)

# Collect source files
set(PROJECT_SOURCES
    src/main.cpp
    src/engine/Allocator.cpp
    src/engine/AssertHandler.cpp
    src/engine/Render.cpp
    src/engine/Engine.cpp
    src/engine/Physics.cpp
    src/engine/AssetFactory.cpp
    src/engine/Window.cpp
    src/engine/Mutex.cpp
    src/engine/Thread.cpp
    src/engine/Thread.S
    src/engine/Input.cpp
)
if(APPLE)
    list(APPEND PROJECT_SOURCES src/engine/Window.mm)
endif()

add_executable(zahlen ${PROJECT_SOURCES})
target_compile_definitions(zahlen PRIVATE 
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

target_link_libraries(zahlen PRIVATE LLGL Jolt LuaJIT)

if(APPLE)
    target_link_libraries(zahlen PRIVATE "-framework Cocoa" "-framework CoreVideo" "-framework Metal" "-framework MetalKit")
endif()

target_include_directories(zahlen SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
    ${CMAKE_SOURCE_DIR}/extern/LLGL/include
)
target_include_directories(zahlen PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/extern/LuaJIT/src
)

# --- Compiler Flags Fix ---
if(MSVC)
  target_compile_options(zahlen PRIVATE /W4 /wd4201 /Zc:preprocessor)
else()
  # For clang++ (GNU driver) 
  target_compile_options(zahlen PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
  
  if(WIN32)
    # Fix for nameless structs/unions in Windows SDK
    target_compile_options(zahlen PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter)
    # Ensure we use the correct CRT for Clang on Windows
    target_compile_options(zahlen PRIVATE -D_CRT_SECURE_NO_WARNINGS)
  endif()
endif()

# --- COPY RESOURCES AND SCRIPTS ---
add_custom_command(TARGET zahlen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/resources
        $<TARGET_FILE_DIR:zahlen>/resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/scripts
        $<TARGET_FILE_DIR:zahlen>/scripts
    COMMENT "Copying resources and scripts..."
)