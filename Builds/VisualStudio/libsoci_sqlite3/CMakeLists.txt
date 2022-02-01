set(PROJECT_NAME libsoci_sqlite3)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/soci/src/backends/sqlite3/common.h"
    "../../../lib/soci/src/backends/sqlite3/soci-sqlite3.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/soci/src/backends/sqlite3/blob.cpp"
    "../../../lib/soci/src/backends/sqlite3/common.cpp"
    "../../../lib/soci/src/backends/sqlite3/factory.cpp"
    "../../../lib/soci/src/backends/sqlite3/row-id.cpp"
    "../../../lib/soci/src/backends/sqlite3/session.cpp"
    "../../../lib/soci/src/backends/sqlite3/standard-into-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/standard-use-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/statement.cpp"
    "../../../lib/soci/src/backends/sqlite3/vector-into-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/vector-use-type.cpp"
)
source_group("Source Files" FILES ${Source_Files})

set(ALL_FILES
    ${Header_Files}
    ${Source_Files}
)

################################################################################
# Target
################################################################################
add_library(${PROJECT_NAME} STATIC ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE libsoci_sqlite3)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/sqlite"
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
            /Gm-;
            /Od
        >
        $<$<CONFIG:DebugNoPostgres>:
            /Gm-;
            /Od
        >
        $<$<CONFIG:Release>:
            /O2;
            /Oi;
            /Gy
        >
        /MP;
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
            /MACHINE:X64
        >
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()



























set(PROJECT_NAME libsoci_sqlite3)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/soci/src/backends/sqlite3/common.h"
    "../../../lib/soci/src/backends/sqlite3/soci-sqlite3.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/soci/src/backends/sqlite3/blob.cpp"
    "../../../lib/soci/src/backends/sqlite3/common.cpp"
    "../../../lib/soci/src/backends/sqlite3/factory.cpp"
    "../../../lib/soci/src/backends/sqlite3/row-id.cpp"
    "../../../lib/soci/src/backends/sqlite3/session.cpp"
    "../../../lib/soci/src/backends/sqlite3/standard-into-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/standard-use-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/statement.cpp"
    "../../../lib/soci/src/backends/sqlite3/vector-into-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/vector-use-type.cpp"
)
source_group("Source Files" FILES ${Source_Files})

set(ALL_FILES
    ${Header_Files}
    ${Source_Files}
)

################################################################################
# Target
################################################################################
add_library(${PROJECT_NAME} STATIC ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE libsoci_sqlite3)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/sqlite"
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
            /Gm-;
            /Od
        >
        $<$<CONFIG:DebugNoPostgres>:
            /Gm-;
            /Od
        >
        $<$<CONFIG:Release>:
            /O2;
            /Oi;
            /Gy
        >
        /MP;
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
            /MACHINE:X64
        >
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()



























set(PROJECT_NAME libsoci_sqlite3)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/soci/src/backends/sqlite3/common.h"
    "../../../lib/soci/src/backends/sqlite3/soci-sqlite3.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/soci/src/backends/sqlite3/blob.cpp"
    "../../../lib/soci/src/backends/sqlite3/common.cpp"
    "../../../lib/soci/src/backends/sqlite3/factory.cpp"
    "../../../lib/soci/src/backends/sqlite3/row-id.cpp"
    "../../../lib/soci/src/backends/sqlite3/session.cpp"
    "../../../lib/soci/src/backends/sqlite3/standard-into-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/standard-use-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/statement.cpp"
    "../../../lib/soci/src/backends/sqlite3/vector-into-type.cpp"
    "../../../lib/soci/src/backends/sqlite3/vector-use-type.cpp"
)
source_group("Source Files" FILES ${Source_Files})

set(ALL_FILES
    ${Header_Files}
    ${Source_Files}
)

################################################################################
# Target
################################################################################
add_library(${PROJECT_NAME} STATIC ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE libsoci_sqlite3)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/sqlite"
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
            /Gm-;
            /Od
        >
        $<$<CONFIG:DebugNoPostgres>:
            /Gm-;
            /Od
        >
        $<$<CONFIG:Release>:
            /O2;
            /Oi;
            /Gy
        >
        /MP;
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
            /MACHINE:X64
        >
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()

