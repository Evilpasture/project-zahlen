# Helper script to safely create symlinks, handling existing paths gracefully
# Usage: cmake -D SRC=<source> -D DST=<destination> -P CreateSymlinks.cmake

function(create_symlink_safe SRC DST)
    # Aggressively remove any existing path (symlink or directory) first
    # using system rm, not CMake's file() command which is unreliable with symlinks
    execute_process(
        COMMAND rm -rf "${DST}"
        ERROR_QUIET
        OUTPUT_QUIET
        RESULT_VARIABLE RM_RESULT
    )
    
    # Small delay to ensure filesystem is consistent
    execute_process(COMMAND sleep 0.1 ERROR_QUIET OUTPUT_QUIET)
    
    # Now create the symlink - if this fails, it's a real error
    if(NOT EXISTS "${DST}")
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
