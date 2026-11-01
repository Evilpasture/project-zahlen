# zpkg -- the Zahlen toolchain manager.
#
# This is a STANDALONE project on purpose, and this file is where that decision
# is enforced:
#
#   * It is never add_subdirectory()-ed into the engine build. There,
#     cmake/Sanitizers.cmake calls add_compile_options(-fsanitize=...) from the
#     top level, so an in-tree zpkg would need libasan.so at run time on a
#     developer's machine in the asan variant.
#   * It does not link zahlen_engine, does not enable reflection, and does not
#     inherit -mavx2/-mfma. A manager that only runs on the CPU it installs
#     packages for is not a manager: the first machine that needs zpkg is the one
#     with no toolchain yet.
#   * On GCC it links libstdc++ statically, so zpkg does not care about the
#     libstdc++ ABI question the packages are so careful about.
#
# Build it on its own:
#     cmake -S utils/zpkg -B build-zpkg -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-zpkg && build-zpkg/zpkg --version
cmake_minimum_required(VERSION 3.28)
project(zpkg LANGUAGES CXX VERSION 2.0.0)

# C++23, not C++20: the error path is std::expected (see include/zpkg/zpkg.hpp),
# and this project does not throw. Where the engine is conservative about the
# standard because it ships a runtime, a host tool can take the newer library.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ZPKG_BUILD_TESTS "Build zpkg's own test suite" ON)
option(ZPKG_ENABLE_SIGNATURE "Verify ed25519 signatures over SHA256SUMS (OpenSSL)" ON)
option(ZPKG_ENABLE_HTTP "Install from https remotes (libcurl)" ON)
option(ZPKG_ENABLE_ZSTD "Read .tar.zst archives (libzstd)" ON)
set(ZPKG_SIMDJSON_DIR "" CACHE PATH "Directory with the amalgamated simdjson.h/simdjson.cpp")

add_library(zpkg_core OBJECT
    src/support.cpp
    src/json.cpp
    src/tar.cpp
    src/store.cpp
    src/sign.cpp
    src/remote.cpp)
target_include_directories(zpkg_core PUBLIC include)

# --- JSON -----------------------------------------------------------------
# simdjson is already a hard dependency of the engine's Serialization package, so
# requiring it here adds nothing to the project. The fallbacks exist for people
# building the manager on a machine that has no Zahlen toolchain yet.
find_package(simdjson CONFIG QUIET)
if(simdjson_FOUND)
    target_link_libraries(zpkg_core PUBLIC simdjson::simdjson)
elseif(ZPKG_SIMDJSON_DIR)
    target_sources(zpkg_core PRIVATE "${ZPKG_SIMDJSON_DIR}/simdjson.cpp")
    target_include_directories(zpkg_core PUBLIC "${ZPKG_SIMDJSON_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../extern/simdjson/CMakeLists.txt")
    set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../extern/simdjson"
        "${CMAKE_CURRENT_BINARY_DIR}/simdjson" EXCLUDE_FROM_ALL)
    target_link_libraries(zpkg_core PUBLIC simdjson)
else()
    message(FATAL_ERROR
        "zpkg needs simdjson. Install it as a CMake package, pass "
        "-DZPKG_SIMDJSON_DIR=<dir with the amalgamated simdjson.h/simdjson.cpp>, "
        "or initialise extern/simdjson.")
endif()

# --- compression ----------------------------------------------------------
# zlib is on every target platform including Windows. zstd is optional: a build
# host without it can still consume the .tar.gz artifacts.
find_package(ZLIB REQUIRED)
target_link_libraries(zpkg_core PUBLIC ZLIB::ZLIB)

if(ZPKG_ENABLE_ZSTD)
    find_package(zstd CONFIG QUIET)
    if(zstd_FOUND)
        target_link_libraries(zpkg_core PUBLIC zstd::libzstd_shared)
        set(ZPKG_ZSTD_OK TRUE)
    else()
        find_path(ZSTD_INCLUDE_DIR zstd.h)
        find_library(ZSTD_LIBRARY NAMES zstd)
        if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
            target_include_directories(zpkg_core PUBLIC "${ZSTD_INCLUDE_DIR}")
            target_link_libraries(zpkg_core PUBLIC "${ZSTD_LIBRARY}")
            set(ZPKG_ZSTD_OK TRUE)
        endif()
    endif()
    if(ZPKG_ZSTD_OK)
        target_compile_definitions(zpkg_core PUBLIC ZPKG_HAVE_ZSTD=1)
    else()
        message(STATUS "zpkg: libzstd not found; .tar.zst archives will be refused")
    endif()
endif()

# --- signatures -----------------------------------------------------------
if(ZPKG_ENABLE_SIGNATURE)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_compile_definitions(zpkg_core PUBLIC ZPKG_HAVE_OPENSSL=1)
        target_link_libraries(zpkg_core PUBLIC OpenSSL::Crypto)
    else()
        message(STATUS "zpkg: OpenSSL not found; built without signature verification")
    endif()
endif()

# --- remotes --------------------------------------------------------------
if(ZPKG_ENABLE_HTTP)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_compile_definitions(zpkg_core PUBLIC ZPKG_HAVE_CURL=1)
        target_link_libraries(zpkg_core PUBLIC CURL::libcurl)
    else()
        message(STATUS "zpkg: libcurl not found; only local and file:// remotes are available")
    endif()
endif()

# --- executable -----------------------------------------------------------
add_executable(zpkg src/main.cpp)
target_link_libraries(zpkg PRIVATE zpkg_core)

if(WIN32)
    # A per-user tool must not need a redistributable to start.
    set_property(TARGET zpkg zpkg_core PROPERTY
        CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(zpkg_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX
        WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(zpkg_core PUBLIC shell32 advapi32)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(zpkg PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(MSVC)
    target_compile_options(zpkg_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(zpkg PRIVATE /W4 /permissive- /utf-8)
else()
    set(ZPKG_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    # -fno-exceptions is the point, not an optimisation: it makes "no throwing"
    # a property the compiler checks instead of a promise in a comment. It is
    # also what zahlen_engine passes down to zcook, which is why that tool has
    # never had a throw in it.
    target_compile_options(zpkg_core PRIVATE ${ZPKG_WARNINGS} -fno-exceptions -fno-rtti)
    target_compile_options(zpkg PRIVATE ${ZPKG_WARNINGS} -fno-exceptions -fno-rtti)
endif()

# --- tests ----------------------------------------------------------------
if(ZPKG_BUILD_TESTS)
    enable_testing()
    add_executable(zpkg_tests tests/main.cpp)
    target_link_libraries(zpkg_tests PRIVATE zpkg_core)
    if(MSVC)
        target_compile_options(zpkg_tests PRIVATE /W4 /utf-8)
    else()
        # The tar fixtures use partial aggregate initialisation on purpose.
        target_compile_options(zpkg_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    endif()
    add_test(NAME zpkg.unit COMMAND zpkg_tests)
endif()
