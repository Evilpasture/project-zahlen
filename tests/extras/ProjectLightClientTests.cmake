# Optional config/launch parsing tests for the native project-light client.
if(TARGET zahlen_project_light_client)
    set(TARGET_NAME TestProjectLightConfig)
    add_executable(${TARGET_NAME} TestProjectLightConfig.cpp)

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_project_light_client
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/extras
    )

    zahlen_enable_reflection(${TARGET_NAME})

    if(USE_SANITIZERS)
        target_compile_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
    endif()

    zahlen_register_extras_test(${TARGET_NAME})
endif()
