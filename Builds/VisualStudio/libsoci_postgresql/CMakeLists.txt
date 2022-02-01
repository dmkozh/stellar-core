set(PROJECT_NAME libsoci_postgresql)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/soci/src/backends/postgresql/common.h"
    "../../../lib/soci/src/backends/postgresql/soci-postgresql.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/soci/src/backends/postgresql/blob.cpp"
    "../../../lib/soci/src/backends/postgresql/common.cpp"
    "../../../lib/soci/src/backends/postgresql/error.cpp"
    "../../../lib/soci/src/backends/postgresql/factory.cpp"
    "../../../lib/soci/src/backends/postgresql/row-id.cpp"
    "../../../lib/soci/src/backends/postgresql/session.cpp"
    "../../../lib/soci/src/backends/postgresql/standard-into-type.cpp"
    "../../../lib/soci/src/backends/postgresql/standard-use-type.cpp"
    "../../../lib/soci/src/backends/postgresql/statement.cpp"
    "../../../lib/soci/src/backends/postgresql/vector-into-type.cpp"
    "../../../lib/soci/src/backends/postgresql/vector-use-type.cpp"
)
source_group("Source Files" FILES ${Source_Files})

set(ALL_FILES
    ${Header_Files}
    ${Source_Files}
)

################################################################################
# Target
################################################################################
add_executable(${PROJECT_NAME} ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE libsoci_postgresql)

set_target_properties(${PROJECT_NAME} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE         "TRUE"
)
################################################################################
# MSVC runtime library
################################################################################
get_property(MSVC_RUNTIME_LIBRARY_DEFAULT TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY)
string(CONCAT "MSVC_RUNTIME_LIBRARY_STR"
    $<$<CONFIG:Debug>:
        MultiThreadedDebugDLL
    >
    $<$<CONFIG:DebugNoPostgres>:
        MultiThreadedDebugDLL
    >
    $<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:DebugNoPostgres>>>:${MSVC_RUNTIME_LIBRARY_DEFAULT}>
)
set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR})

################################################################################
# Include directories
################################################################################
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/soci/src/core;"
    "c:/Program Files/PostgreSQL/9.5/include"
)

################################################################################
# Compile definitions
################################################################################
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "_CRT_SECURE_NO_WARNINGS;"
    "_MBCS"
)

################################################################################
# Compile and link options
################################################################################
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:
            /MP;
            /Gm-;
            /Od
        >
        $<$<CONFIG:DebugNoPostgres>:
            /MP;
            /Gm-;
            /Od
        >
        $<$<CONFIG:Release>:
            /O2;
            /Oi;
            /Gy
        >
        /sdl;
        /W3;
        ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
        ${DEFAULT_CXX_EXCEPTION_HANDLING}
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:
            /MACHINE:X64
        >
        $<$<CONFIG:DebugNoPostgres>:
            /DEBUG;
            /MACHINE:X64
        >
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()

################################################################################
# Dependencies
################################################################################
set(ADDITIONAL_LIBRARY_DEPENDENCIES
    "$<$<CONFIG:Debug>:"
        "C:\Program Files\PostgreSQL\9.5\lib\libpq"
    ">"
    "$<$<CONFIG:DebugNoPostgres>:"
        "C:\Program Files\PostgreSQL\9.5\lib\libpq"
    ">"
)
target_link_libraries(${PROJECT_NAME} PRIVATE "${ADDITIONAL_LIBRARY_DEPENDENCIES}")

