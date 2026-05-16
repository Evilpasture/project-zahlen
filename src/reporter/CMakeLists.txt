# src/reporter/CMakeLists.txt

add_executable(ZahlenReporter main.cpp)

# Ensure it uses the same C++23 standard as the engine
target_compile_features(ZahlenReporter PRIVATE cxx_std_23)

# We want the reporter to be in the same folder as the engine so it can find scripts/resources
set_target_properties(ZahlenReporter PROPERTIES 
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:zahlen>"
)

# Platform specific linking
if(WIN32)
    # Required for MiniDumpWriteDump and Sym... functions
    target_link_libraries(ZahlenReporter PRIVATE dbghelp)
    
    # Force Console subsystem so we can see the report output
    target_link_options(ZahlenReporter PRIVATE -Xlinker /subsystem:console)
endif()

# Apply the same warning/optimization flags as the root if desired
if(NOT MSVC)
    target_compile_options(ZahlenReporter PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
endif()