# cmake/AppleShim.cmake
# Ensures Homebrew-installed Clang properly finds Apple System Frameworks/Headers

if(APPLE)
    # 1. Locate the SDK
    execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(MACOS_SDK_PATH)
        message(STATUS "Apple Shim: Using SDK at ${MACOS_SDK_PATH}")
        
        # 2. Force these into the compiler flags so the compiler "sees" the system
        # We use CMAKE_REQUIRED_FLAGS to ensure feature checks also work
        set(CMAKE_SYSROOT "${MACOS_SDK_PATH}" CACHE PATH "System root" FORCE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isysroot ${MACOS_SDK_PATH}" CACHE STRING "" FORCE)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -isysroot ${MACOS_SDK_PATH}" CACHE STRING "" FORCE)
        
        # 3. Ensure linking against the correct C++ standard library
        # Homebrew LLVM sometimes defaults to its own, but we want system-provided one
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++" CACHE STRING "" FORCE)
    else()
        message(WARNING "Apple Shim: Could not locate macOS SDK via xcrun. Builds may fail.")
    endif()
endif()