# tools/zshader/CMakeLists.txt
#
# The catalog generator, as its own directory: it is the one target that has to
# exist before the renderer's sources can be compiled at all, and it is built
# from sources that go through the transpiler -- which registers source
# directories with include_directories() for the transpiled copies to find their
# headers through. Scoping that to this directory is why the target is not
# declared inline in cmake/ShaderCompilation.cmake, where it used to live.

# Four translation units: the model they share (ZShader.hpp), the command line
# that fills it (main.cpp), the argument and file helpers (ZShader.cpp), the
# SPIRV-Reflect pass (Reflect.cpp) and the two emitters (Emit.cpp).
set(ZHLN_ZSHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ZShader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Reflect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Emit.cpp"
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

target_link_libraries(zshader PRIVATE
    spirv_reflect   # the reflection pass itself
    Vulkan-Headers  # <vulkan/vulkan_core.h>: the enumerators Reflect.cpp names
    zahlen_warnings # the same warning set as every other target in the project
)

# The engine's headers, read-only: Zahlen/Core/Reflection for EnumToString and
# Zahlen/Core/Ranges for the ranges the tool uses where the engine does.
target_include_directories(zshader PRIVATE "${CMAKE_SOURCE_DIR}/include")

target_include_directories(zshader SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/SPIRV-Reflect
    ${CMAKE_SOURCE_DIR}/third_party/SPIRV-Reflect/include
)

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
