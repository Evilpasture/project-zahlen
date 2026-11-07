# One provenance decision for libslang: its public headers and its shared
# library. Order: host config package -> slangpy wheel -> Linux release ->
# pinned source.
#
# zshader links libslang once and is the ONLY shader compiler driver. There is
# no compiler executable to find, no compiler/library version cross-check, no
# ELF/Mach-O probe of a binary that is not part of the build, and no version
# gate: the same library answers both "what does this module declare" and "what
# are its bytes". On ELF/Mach-O the build tree's RUNPATH loads it; on Windows
# its directory goes on PATH for the duration of the tool command.

option(ZHLN_SLANG_VENDORED "Force the pinned Slang source build" OFF)

# Config packages may advertise the *parent* of the real header directory (the
# Vulkan SDK ships include/slang/ while naming include/), and built trees use
# $<BUILD_INTERFACE:...> wrappers. Resolve both shapes once, here.
function(_zahlen_slang_headers target out)
    set(_found "")
    get_target_property(_includes "${target}" INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_inc IN LISTS _includes)
        set(_dir "${_inc}")
        if(_dir MATCHES "^\\$<(BUILD_INTERFACE|INSTALL_INTERFACE):(.*)>$")
            set(_dir "${CMAKE_MATCH_2}")
        endif()
        foreach(_candidate IN ITEMS "${_dir}" "${_dir}/slang" "${_dir}/shader-slang")
            if(EXISTS "${_candidate}/slang.h" AND
               EXISTS "${_candidate}/slang-com-helper.h" AND
               EXISTS "${_candidate}/slang-com-ptr.h")
                set(_found "${_candidate}")
                break()
            endif()
        endforeach()
        if(_found)
            break()
        endif()
    endforeach()
    set(${out} "${_found}" PARENT_SCOPE)
endfunction()

# A directory holding a release/wheel: accept it when it has both parts and
# describe them as one imported target. No probe of anything else in the tree.
function(_zahlen_try_slang root provenance)
    if(ZHLN_SLANG_TARGET OR NOT IS_DIRECTORY "${root}")
        return()
    endif()
    set(_inc "")
    foreach(_p IN ITEMS "${root}/include/shader-slang" "${root}/include/slang" "${root}/include" "${root}")
        if(EXISTS "${_p}/slang.h" AND EXISTS "${_p}/slang-com-helper.h" AND EXISTS "${_p}/slang-com-ptr.h")
            set(_inc "${_p}")
            break()
        endif()
    endforeach()
    if(APPLE)
        file(GLOB _libs "${root}/lib/libslang-compiler*.dylib" "${root}/libslang-compiler*.dylib")
    elseif(WIN32)
        file(GLOB _libs "${root}/lib/slang*.lib" "${root}/slang*.lib")
    else()
        file(GLOB _libs "${root}/lib/libslang-compiler.so*" "${root}/lib/libslang.so*" "${root}/libslang-compiler.so*")
    endif()
    list(SORT _libs)
    set(_lib "")
    if(_libs)
        list(GET _libs 0 _lib)
    endif()
    if(NOT _inc OR NOT _lib)
        message(STATUS "libslang: reject ${provenance} ${root}: incomplete library/public headers")
        return()
    endif()
    add_library(zahlen_slang_selected UNKNOWN IMPORTED GLOBAL)
    set_target_properties(zahlen_slang_selected PROPERTIES
        IMPORTED_LOCATION "${_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}")
    set(ZHLN_SLANG_TARGET zahlen_slang_selected PARENT_SCOPE)
    set(ZHLN_SLANG_INCLUDE_DIR "${_inc}" PARENT_SCOPE)
    message(STATUS "libslang: selected ${provenance} (${root})")
endfunction()

set(ZHLN_SLANG_TARGET "")
set(ZHLN_SLANG_INCLUDE_DIR "")
if(NOT ZHLN_SLANG_VENDORED)
    # Discover config packages through CMake's native search. Ignore only stale
    # package-directory results owned by previous fallbacks.
    if(slang_DIR MATCHES "/_deps/slang-release/|/extern/slang/")
        unset(slang_DIR CACHE)
        unset(slang_DIR)
    endif()
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/SlangHost"
                     "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/zahlen-slang-host")
    if(ZHLN_SLANG_TARGET)
        _zahlen_slang_headers("${ZHLN_SLANG_TARGET}" ZHLN_SLANG_INCLUDE_DIR)
    endif()
    if(NOT ZHLN_SLANG_TARGET AND Python3_EXECUTABLE)
        # Local package inspection only. No pip, download or Python network I/O.
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c
            "import importlib.util; s=importlib.util.find_spec('slangpy'); print(next(iter(s.submodule_search_locations), '') if s and s.submodule_search_locations else '')"
            OUTPUT_VARIABLE _wheel OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 15)
        if(_wheel)
            _zahlen_try_slang("${_wheel}" slangpy)
        endif()
    endif()
    if(NOT ZHLN_SLANG_TARGET AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The downloader is only reached after both local candidates fail.
        include(SlangPrebuilt)
        if(ZHLN_SLANG_PREBUILT_DIR)
            _zahlen_try_slang("${ZHLN_SLANG_PREBUILT_DIR}" linux-release)
        endif()
    endif()
endif()
if(NOT ZHLN_SLANG_TARGET)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/slang/CMakeLists.txt")
        message(FATAL_ERROR "No libslang found; initialize extern/slang with git submodule update --init --recursive")
    endif()
    if(NOT ZHLN_HAS_PYTHON)
        message(FATAL_ERROR "Vendored Slang code generation requires Python 3")
    endif()
    message(STATUS "libslang: selected vendored source ${CMAKE_CURRENT_SOURCE_DIR}/extern/slang")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/extern/slang" EXCLUDE_FROM_ALL)
    if(TARGET slang::slang)
        set(ZHLN_SLANG_TARGET slang::slang)
    elseif(TARGET slang)
        set(ZHLN_SLANG_TARGET slang)
    else()
        message(FATAL_ERROR "Vendored Slang provides no library target")
    endif()
    _zahlen_slang_headers("${ZHLN_SLANG_TARGET}" ZHLN_SLANG_INCLUDE_DIR)
    # Deliberately NO slangc target here: SLANG_ENABLE_SLANGC=OFF (see
    # ShaderCompilation.cmake) keeps the standalone compiler out of the build.
endif()
if(NOT ZHLN_SLANG_INCLUDE_DIR)
    message(FATAL_ERROR
        "The libslang target ${ZHLN_SLANG_TARGET} advertises no directory containing "
        "slang.h, slang-com-helper.h and slang-com-ptr.h. Delete the build directory "
        "and point CMake at a complete libslang install.")
endif()
message(STATUS "libslang headers: ${ZHLN_SLANG_INCLUDE_DIR}")
