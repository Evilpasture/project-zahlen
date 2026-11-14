# utils/zshader/CMakeLists.txt
#
# The catalog generator, as its own directory: it is the one target that has to
# exist before the renderer's sources can be compiled at all, and it is built
# from sources that go through the transpiler -- which registers source
# directories with include_directories() for the transpiled copies to find their
# headers through. Scoping that to this directory is why the target is not
# declared inline in cmake/ShaderCompilation.cmake, where it used to live.

# Eight translation units: the model they share (ZShader.hpp), the command line
# (main.cpp), the argument and file helpers (ZShader.cpp), the batch manifest
# driver (Manifest.cpp), the Slang compile+reflect pass (SlangReflect.cpp,
# driven per-module by Reflect.cpp), the emitters (Emit.cpp), the standalone
# slangc/dx compiler front doors (Compile.cpp) and the gpu-types mode
# (GpuTypes.cpp). The tool is the build's only shader compiler driver: no
# slangc binary is located, invoked or version-checked anywhere.
set(ZHLN_ZSHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ZShader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Reflect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SlangReflect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Emit.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Compile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/GpuTypes.cpp"
)

# The tool reads the engine's headers -- Reflect::EnumToString for the enumerator
# names it writes, ZHLN::Ranges for the walk over its arguments -- but it cannot
# link the engine: zahlen_engine links zahlen_render, which compiles the catalog
# this tool writes, so an edge back would be a cycle.
#
# It used to run its sources through zahlen_transpile_sources() for the benefit
# of a compiler without -freflection. That path is gone: the reflection headers
# require -freflection outright (Core/Reflection/Core.hpp takes the no-reflection
# branch only so the *error* is comprehensible), the root CMakeLists rejects
# toolchains that cannot do it, and zshader is compiled with
# zahlen_enable_reflection like every other target that reads a reflected
# header. Wrapping in the transpiler here also had a side effect nobody wanted:
# under COMPILER_HAS_REFLECTION the function returns its arguments untouched but
# the call site still reads as if a rewrite might happen.
#
# The reflection flags themselves -- -freflection, -fannotation-attributes and
# __cpp_impl_reflection -- arrive from zahlen_enable_reflection below, which is
# applied after this directory is configured. That is why the root CMakeLists
# names zshader in its own list rather than calling the function here.

set(ZHLN_ZSHADER_IMAGE_COOK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ImageCook.cpp")
set_source_files_properties("${ZHLN_ZSHADER_IMAGE_COOK_SOURCE}" PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "STBI_ONLY_PNG;STBI_NO_STDIO;STBI_NO_LINEAR;STBI_NO_HDR"
)

add_executable(zshader
    ${ZHLN_ZSHADER_SOURCES}
    "${ZHLN_ZSHADER_IMAGE_COOK_SOURCE}"
)
zahlen_set_bin_output(zshader)

# libslang comes from cmake/ShaderCompilation.cmake, which is the file that
# adds this directory: a config package (Vulkan SDK, Slang release) or the
# vendored tree, whichever the provision there settled on.
if(NOT DEFINED ZHLN_SLANG_TARGET)
    message(FATAL_ERROR "ZHLN_SLANG_TARGET is not set; utils/zshader is only added from cmake/ShaderCompilation.cmake")
endif()

target_link_libraries(zshader PRIVATE
    Vulkan-Headers       # <vulkan/vulkan_core.h>: the enumerators Reflect.cpp names
    zahlen_warnings      # the same warning set as every other target in the project
    ${ZHLN_SLANG_TARGET}
    $<$<AND:$<BOOL:${MINGW}>,$<CXX_COMPILER_ID:GNU>>:stdc++exp> # libslang: both reflection passes compile in-process (catalog + gpu-types)
)

# Slang's own headers do not survive this project's warning set (old-style
# casts throughout slang.h), so they arrive as system includes. The directory is
# ZHLN_SLANG_INCLUDE_DIR, resolved once by cmake/SlangResolve.cmake: a config
# package does not always advertise the directory that actually holds them (the
# Vulkan SDK names its parent while shipping them in its slang/ subdirectory).
# The tool RUNS during the build as well, so libslang must load without an
# install step: the build tree's own RPATH carries the library's directory.
# APPEND, not set -- CMAKE_BUILD_RPATH may already hold the libc++ runtime
# (cmake/HostToolRuntime.cmake), and a plain set would drop it.
target_include_directories(zshader SYSTEM PRIVATE
    ${ZHLN_SLANG_INCLUDE_DIR}
    "${CMAKE_SOURCE_DIR}/extern/stb"
)
set_property(TARGET zshader APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:${ZHLN_SLANG_TARGET}>")
if(TARGET slang-glslang)
    set_property(TARGET zshader APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:slang-glslang>")
    add_dependencies(zshader slang-glslang)
endif()

# The engine's headers, read-only: Zahlen/Core/Reflection for EnumToString and
# Zahlen/Core/Ranges for the ranges the tool uses where the engine does.
target_include_directories(zshader PRIVATE "${CMAKE_SOURCE_DIR}/include")

# The engine publishes these on its own target (CMakeLists.txt:984) and the tool
# cannot inherit them, so it states them: every engine header this tool includes
# is written for a build without exceptions or RTTI.
target_compile_options(zshader PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)

# The tool is the build's critical path: the catalog and gpu-types commands
# cannot run until its eight translation units are built, and every downstream
# consumer of the generated headers queues behind them. What the six actually
# share is ZShader.hpp everywhere, slang.h in three of them, and vulkan_core.h
# (Reflect.cpp reads the enumerators); those parse once into the PCH.
#
# Deliberately NOT here: the RHI umbrella. Emit.cpp's mentions of
# Rendering.hpp and pipeline/ShaderProgram.hpp are TEXT -- lines the emitter
# writes into the generated ShaderBindings.hpp/ShaderBytecode.cpp, sitting in
# raw-string templates at Emit.cpp:129 and :210 -- not headers it consumes
# (grep matches them anyway, which is how a PCH once got built around them).
# zshader has no src/vulkan or VMA include path, and forcing the umbrella in
# drags VulkanHeader.hpp's Volk include chain into a target that links
# neither -- which is precisely the pair of failures this block once produced.
# vulkan_core.h IS safe here: with no Rendering.hpp in the PCH there is no
# volk in it either, so real prototypes first is the only order present --
# exactly what Reflect.cpp compiles today.
#
# Gated exactly like every other PCH in the tree: ZHLN_ENABLE_PCH is a build
# option, and clang-p2996's PCH ASTReader crashes on P2996 reflection
# expressions, so the compiler gate is the one src/render, src/physics and
# src/vulkan already use. Ungated, this target ignored both.
if(ZHLN_ENABLE_PCH AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_precompile_headers(zshader PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/ZShader.hpp"
        "<slang.h>"
        "<vulkan/vulkan_core.h>"
    )
endif()

