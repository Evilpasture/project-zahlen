# ----------------------------------------------------------------------------
# zahlen_render — Vulkan/VMA leaf module
# No engine dependencies. Vulkan + VMA only.
# ----------------------------------------------------------------------------

find_package(Vulkan REQUIRED)

add_library(zahlen_render STATIC
    RenderCore.c
    RenderCore.h
    RenderCore.hpp
    Allocator.hpp
    Utils.h
)

set_source_files_properties(RenderCore.c PROPERTIES
    LANGUAGE C
    C_STANDARD 23
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_compile_features(zahlen_render PUBLIC cxx_std_23)
set_target_properties(zahlen_render PROPERTIES CXX_EXTENSIONS OFF)

target_link_libraries(zahlen_render PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
)

target_include_directories(zahlen_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(zahlen_render PRIVATE
    VMA_IMPLEMENTATION
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zahlen_render PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
endif()