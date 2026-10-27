# Optional config/launch parsing tests for the native project-light client.
if(TARGET zahlen_project_light_client)
    # The replication bridge on its own: components, the uid -> entity index, the
    # snapshot parser and the record graph. No socket and no renderer, so a
    # regression here names itself instead of needing a server to reproduce.
    set(TARGET_NAME TestProjectLightReplication)
    zahlen_wrap_ztest_sources(_pl_rep_sources _pl_rep_files TestProjectLightReplication.cpp)
    add_executable(${TARGET_NAME}
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
        ${_pl_rep_sources}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        ZHLN_ZTEST_SOURCE_FILES "${_pl_rep_files}"
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_project_light_client
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
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

    set(TARGET_NAME TestProjectLightConfig)
    zahlen_wrap_ztest_sources(_pl_cfg_sources _pl_cfg_files TestProjectLightConfig.cpp)
    add_executable(${TARGET_NAME}
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
        ${_pl_cfg_sources}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        ZHLN_ZTEST_SOURCE_FILES "${_pl_cfg_files}"
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_project_light_client
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
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
