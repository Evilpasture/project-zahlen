# tools/zshader/CMakeLists.txt
#
# The catalog generator, as its own directory: it is the one target that has to
# exist before the renderer's sources can be compiled at all, and it is built
# from sources that go through the transpiler -- which registers source
# directories with include_directories() for the transpiled copies to find their
# headers through. Scoping that to this directory is why the target is not
# declared inline in cmake/ShaderCompilation.cmake, where it used to live.

# Six translation units: the model they share (ZShader.hpp), the command line
# that fills it (main.cpp), the argument and file helpers (ZShader.cpp), the
# Slang reflection pass (SlangReflect.cpp, driven per-module by Reflect.cpp)
# and the two emitters (Emit.cpp) -- plus the gpu-types mode (GpuTypes.cpp),
# which compiles the gpu_abi module in-process and walks its reflected layout
# into the generated host structs in a separate invocation.
set(ZHLN_ZSHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ZShader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Reflect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SlangReflect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Emit.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/GpuTypes.cpp"
)

# The tool reads the engine's headers -- Reflect::EnumToString for the enumerator
# names it writes, ZHLN::Ranges for the walk over its arguments -- but it cannot
# link the engine: zahlen_engine links zahlen_render, which compiles the catalog
# this tool writes, so an edge back would be a cycle. What it shares with the
# engine instead is the build's reflection story, and it shares it the same way:
# through zahlen_transpile_sources(). Under a compiler without -freflection this
# rewrites the EnumToString calls into the switch they stand for, which is the
# difference between a catalog of real names and one whose every descriptor type
# is the sentinel -- Reflect.cpp asks once, up front, and refuses to write the
# second kind (see NamesEnumerators()).
zahlen_transpile_sources(ZHLN_ZSHADER_SOURCES ${ZHLN_ZSHADER_SOURCES})

add_executable(zshader ${ZHLN_ZSHADER_SOURCES})

# libslang comes from cmake/ShaderCompilation.cmake, which is the file that
# adds this directory: a config package (Vulkan SDK, Slang release) or the
# vendored tree, whichever the provision there settled on.
if(NOT DEFINED ZHLN_SLANG_TARGET)
    message(FATAL_ERROR "ZHLN_SLANG_TARGET is not set; tools/zshader is only added from cmake/ShaderCompilation.cmake")
endif()

target_link_libraries(zshader PRIVATE
    Vulkan-Headers       # <vulkan/vulkan_core.h>: the enumerators Reflect.cpp names
    zahlen_warnings      # the same warning set as every other target in the project
    ${ZHLN_SLANG_TARGET}
    $<$<BOOL:${MINGW}>:stdc++exp> # libslang: both reflection passes compile in-process (catalog + gpu-types); MinGW's libstdc++ needs the library named, other environments resolve it -- off MinGW the genex is empty, so it needs no if() gate
)

# Slang's own headers do not survive this project's warning set (old-style
# casts throughout slang.h), so they arrive as system includes however the
# provision found them. The directory is ZHLN_SLANG_INCLUDE_DIR, which
# cmake/ShaderCompilation.cmake verified actually resolves them -- the
# includedir a config package advertises is not always that (the Vulkan SDK
# names its parent while shipping the headers in its slang/ subdirectory).
# The tool RUNS during the build as well, so libslang must load without an
# install step: the build tree's own RPATH carries the library's directory.
# APPEND, not set -- CMAKE_BUILD_RPATH may already hold the libc++ runtime
# (cmake/HostToolRuntime.cmake), and a plain set would drop it.
target_include_directories(zshader SYSTEM PRIVATE
    ${ZHLN_SLANG_INCLUDE_DIR}
)
set_property(TARGET zshader APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:${ZHLN_SLANG_TARGET}>")
if(TARGET slang-glslang)
    set_property(TARGET zshader APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:slang-glslang>")
    add_dependencies(zshader slang-glslang)
endif()
if(DEFINED ZHLN_SLANG_GLSLANG_DIR AND NOT "${ZHLN_SLANG_GLSLANG_DIR}" STREQUAL "")
    set_property(TARGET zshader APPEND PROPERTY BUILD_RPATH "${ZHLN_SLANG_GLSLANG_DIR}")
endif()

# The engine's headers, read-only: Zahlen/Core/Reflection for EnumToString and
# Zahlen/Core/Ranges for the ranges the tool uses where the engine does.
target_include_directories(zshader PRIVATE "${CMAKE_SOURCE_DIR}/include")

# Vk::Result's build-tree definition (see cmake/VkResultMirror.cmake): Emit.cpp
# reads the RHI umbrella, which reaches RenderCore.hpp, so the generated dir
# must resolve here too. Stated directly -- the tool needs one generated header,
# not the RHI library, so it does not link zahlen_vulkan for it. Set at the top
# level before cmake/ShaderCompilation.cmake adds this directory.

# The engine publishes these on its own target (CMakeLists.txt:984) and the tool
# cannot inherit them, so it states them: every engine header this tool includes
# is written for a build without exceptions or RTTI.
target_compile_options(zshader PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)

# The reflection flags themselves -- -freflection, -fannotation-attributes and
# __cpp_impl_reflection -- come from the root CMakeLists' zahlen_enable_reflection
# list, where every target that includes a reflection header is named. That
# function does not exist yet while this directory is configured (ShaderCompilation
# is included above it), so the call sits in that list, next to zcook's.
#
# A build without -freflection compiles the stand-ins instead, and Core.hpp's
# static_assert is the reminder that a target using them has to say so. The
# transpiled copies above are what makes them harmless here -- the three
# EnumToString calls, including the one NamesEnumerators() asks its question
# with, are switches by then -- so the tool opts out of the assert the way the
# header asks host-only tooling to. A build that reaches the tool with neither
# is caught at startup instead: NamesEnumerators() refuses to write a catalog
# whose every descriptor type is the sentinel.
if(NOT COMPILER_HAS_REFLECTION)
    target_compile_definitions(zshader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:ZHLN_ALLOW_REFLECTION_STUBS=1>)
endif()
