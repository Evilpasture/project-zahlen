# Helper script to safely create symlinks, handling existing paths gracefully
# Usage: cmake -D SRC=<source> -D DST=<destination> -P CreateSymlinks.cmake

function(create_symlink_safe SRC DST)
    if(IS_SYMLINK "${DST}")
        file(READ_SYMLINK "${DST}" _current_target)
        if("${_current_target}" STREQUAL "${SRC}")
            return()
        endif()
    endif()

    if(EXISTS "${DST}" OR IS_SYMLINK "${DST}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${DST}"
            ERROR_QUIET
            OUTPUT_QUIET
        )
    endif()

    if(NOT EXISTS "${DST}" AND NOT IS_SYMLINK "${DST}")
        file(CREATE_LINK "${SRC}" "${DST}" SYMBOLIC)
        if(EXISTS "${DST}" OR IS_SYMLINK "${DST}")
            message(STATUS "Created symlink: ${DST} -> ${SRC}")
        else()
            message(FATAL_ERROR "Failed to create symlink: ${DST} -> ${SRC}")
        endif()
    else()
        message(FATAL_ERROR "Destination still exists after removal: ${DST}")
    endif()
endfunction()

# Main execution
if(DEFINED SRC AND DEFINED DST)
    create_symlink_safe("${SRC}" "${DST}")
endif()
