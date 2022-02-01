set(PROJECT_NAME libsoci)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/soci/src/core/backend-loader.h"
    "../../../lib/soci/src/core/blob-exchange.h"
    "../../../lib/soci/src/core/blob.h"
    "../../../lib/soci/src/core/connection-parameters.h"
    "../../../lib/soci/src/core/connection-pool.h"
    "../../../lib/soci/src/core/error.h"
    "../../../lib/soci/src/core/exchange-traits.h"
    "../../../lib/soci/src/core/into-type.h"
    "../../../lib/soci/src/core/into.h"
    "../../../lib/soci/src/core/once-temp-type.h"
    "../../../lib/soci/src/core/prepare-temp-type.h"
    "../../../lib/soci/src/core/procedure.h"
    "../../../lib/soci/src/core/query_transformation.h"
    "../../../lib/soci/src/core/ref-counted-prepare-info.h"
    "../../../lib/soci/src/core/ref-counted-statement.h"
    "../../../lib/soci/src/core/row-exchange.h"
    "../../../lib/soci/src/core/row.h"
    "../../../lib/soci/src/core/rowid-exchange.h"
    "../../../lib/soci/src/core/rowid.h"
    "../../../lib/soci/src/core/rowset.h"
    "../../../lib/soci/src/core/session.h"
    "../../../lib/soci/src/core/soci-backend.h"
    "../../../lib/soci/src/core/soci-config.h"
    "../../../lib/soci/src/core/soci-platform.h"
    "../../../lib/soci/src/core/soci-simple.h"
    "../../../lib/soci/src/core/soci.h"
    "../../../lib/soci/src/core/soci_backends_config.h"
    "../../../lib/soci/src/core/statement.h"
    "../../../lib/soci/src/core/transaction.h"
    "../../../lib/soci/src/core/type-conversion-traits.h"
    "../../../lib/soci/src/core/type-conversion.h"
    "../../../lib/soci/src/core/type-holder.h"
    "../../../lib/soci/src/core/type-ptr.h"
    "../../../lib/soci/src/core/unsigned-types.h"
    "../../../lib/soci/src/core/use-type.h"
    "../../../lib/soci/src/core/use.h"
    "../../../lib/soci/src/core/values-exchange.h"
    "../../../lib/soci/src/core/values.h"
    "../../../lib/soci/src/core/version.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/soci/src/core/backend-loader.cpp"
    "../../../lib/soci/src/core/blob.cpp"
    "../../../lib/soci/src/core/connection-parameters.cpp"
    "../../../lib/soci/src/core/connection-pool.cpp"
    "../../../lib/soci/src/core/error.cpp"
    "../../../lib/soci/src/core/into-type.cpp"
    "../../../lib/soci/src/core/once-temp-type.cpp"
    "../../../lib/soci/src/core/prepare-temp-type.cpp"
    "../../../lib/soci/src/core/procedure.cpp"
    "../../../lib/soci/src/core/ref-counted-prepare-info.cpp"
    "../../../lib/soci/src/core/ref-counted-statement.cpp"
    "../../../lib/soci/src/core/row.cpp"
    "../../../lib/soci/src/core/rowid.cpp"
    "../../../lib/soci/src/core/session.cpp"
    "../../../lib/soci/src/core/soci-simple.cpp"
    "../../../lib/soci/src/core/statement.cpp"
    "../../../lib/soci/src/core/transaction.cpp"
    "../../../lib/soci/src/core/use-type.cpp"
    "../../../lib/soci/src/core/values.cpp"
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
set(ROOT_NAMESPACE libsoci)

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
            /MACHINE:X64
        >
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()



























set(PROJECT_NAME libsoci)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/soci/src/core/backend-loader.h"
    "../../../lib/soci/src/core/blob-exchange.h"
    "../../../lib/soci/src/core/blob.h"
    "../../../lib/soci/src/core/connection-parameters.h"
    "../../../lib/soci/src/core/connection-pool.h"
    "../../../lib/soci/src/core/error.h"
    "../../../lib/soci/src/core/exchange-traits.h"
    "../../../lib/soci/src/core/into-type.h"
    "../../../lib/soci/src/core/into.h"
    "../../../lib/soci/src/core/once-temp-type.h"
    "../../../lib/soci/src/core/prepare-temp-type.h"
    "../../../lib/soci/src/core/procedure.h"
    "../../../lib/soci/src/core/query_transformation.h"
    "../../../lib/soci/src/core/ref-counted-prepare-info.h"
    "../../../lib/soci/src/core/ref-counted-statement.h"
    "../../../lib/soci/src/core/row-exchange.h"
    "../../../lib/soci/src/core/row.h"
    "../../../lib/soci/src/core/rowid-exchange.h"
    "../../../lib/soci/src/core/rowid.h"
    "../../../lib/soci/src/core/rowset.h"
    "../../../lib/soci/src/core/session.h"
    "../../../lib/soci/src/core/soci-backend.h"
    "../../../lib/soci/src/core/soci-config.h"
    "../../../lib/soci/src/core/soci-platform.h"
    "../../../lib/soci/src/core/soci-simple.h"
    "../../../lib/soci/src/core/soci.h"
    "../../../lib/soci/src/core/soci_backends_config.h"
    "../../../lib/soci/src/core/statement.h"
    "../../../lib/soci/src/core/transaction.h"
    "../../../lib/soci/src/core/type-conversion-traits.h"
    "../../../lib/soci/src/core/type-conversion.h"
    "../../../lib/soci/src/core/type-holder.h"
    "../../../lib/soci/src/core/type-ptr.h"
    "../../../lib/soci/src/core/unsigned-types.h"
    "../../../lib/soci/src/core/use-type.h"
    "../../../lib/soci/src/core/use.h"
    "../../../lib/soci/src/core/values-exchange.h"
    "../../../lib/soci/src/core/values.h"
    "../../../lib/soci/src/core/version.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/soci/src/core/backend-loader.cpp"
    "../../../lib/soci/src/core/blob.cpp"
    "../../../lib/soci/src/core/connection-parameters.cpp"
    "../../../lib/soci/src/core/connection-pool.cpp"
    "../../../lib/soci/src/core/error.cpp"
    "../../../lib/soci/src/core/into-type.cpp"
    "../../../lib/soci/src/core/once-temp-type.cpp"
    "../../../lib/soci/src/core/prepare-temp-type.cpp"
    "../../../lib/soci/src/core/procedure.cpp"
    "../../../lib/soci/src/core/ref-counted-prepare-info.cpp"
    "../../../lib/soci/src/core/ref-counted-statement.cpp"
    "../../../lib/soci/src/core/row.cpp"
    "../../../lib/soci/src/core/rowid.cpp"
    "../../../lib/soci/src/core/session.cpp"
    "../../../lib/soci/src/core/soci-simple.cpp"
    "../../../lib/soci/src/core/statement.cpp"
    "../../../lib/soci/src/core/transaction.cpp"
    "../../../lib/soci/src/core/use-type.cpp"
    "../../../lib/soci/src/core/values.cpp"
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
set(ROOT_NAMESPACE libsoci)

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
            /MACHINE:X64
        >
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()



























set(PROJECT_NAME libsoci)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/soci/src/core/backend-loader.h"
    "../../../lib/soci/src/core/blob-exchange.h"
    "../../../lib/soci/src/core/blob.h"
    "../../../lib/soci/src/core/connection-parameters.h"
    "../../../lib/soci/src/core/connection-pool.h"
    "../../../lib/soci/src/core/error.h"
    "../../../lib/soci/src/core/exchange-traits.h"
    "../../../lib/soci/src/core/into-type.h"
    "../../../lib/soci/src/core/into.h"
    "../../../lib/soci/src/core/once-temp-type.h"
    "../../../lib/soci/src/core/prepare-temp-type.h"
    "../../../lib/soci/src/core/procedure.h"
    "../../../lib/soci/src/core/query_transformation.h"
    "../../../lib/soci/src/core/ref-counted-prepare-info.h"
    "../../../lib/soci/src/core/ref-counted-statement.h"
    "../../../lib/soci/src/core/row-exchange.h"
    "../../../lib/soci/src/core/row.h"
    "../../../lib/soci/src/core/rowid-exchange.h"
    "../../../lib/soci/src/core/rowid.h"
    "../../../lib/soci/src/core/rowset.h"
    "../../../lib/soci/src/core/session.h"
    "../../../lib/soci/src/core/soci-backend.h"
    "../../../lib/soci/src/core/soci-config.h"
    "../../../lib/soci/src/core/soci-platform.h"
    "../../../lib/soci/src/core/soci-simple.h"
    "../../../lib/soci/src/core/soci.h"
    "../../../lib/soci/src/core/soci_backends_config.h"
    "../../../lib/soci/src/core/statement.h"
    "../../../lib/soci/src/core/transaction.h"
    "../../../lib/soci/src/core/type-conversion-traits.h"
    "../../../lib/soci/src/core/type-conversion.h"
    "../../../lib/soci/src/core/type-holder.h"
    "../../../lib/soci/src/core/type-ptr.h"
    "../../../lib/soci/src/core/unsigned-types.h"
    "../../../lib/soci/src/core/use-type.h"
    "../../../lib/soci/src/core/use.h"
    "../../../lib/soci/src/core/values-exchange.h"
    "../../../lib/soci/src/core/values.h"
    "../../../lib/soci/src/core/version.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/soci/src/core/backend-loader.cpp"
    "../../../lib/soci/src/core/blob.cpp"
    "../../../lib/soci/src/core/connection-parameters.cpp"
    "../../../lib/soci/src/core/connection-pool.cpp"
    "../../../lib/soci/src/core/error.cpp"
    "../../../lib/soci/src/core/into-type.cpp"
    "../../../lib/soci/src/core/once-temp-type.cpp"
    "../../../lib/soci/src/core/prepare-temp-type.cpp"
    "../../../lib/soci/src/core/procedure.cpp"
    "../../../lib/soci/src/core/ref-counted-prepare-info.cpp"
    "../../../lib/soci/src/core/ref-counted-statement.cpp"
    "../../../lib/soci/src/core/row.cpp"
    "../../../lib/soci/src/core/rowid.cpp"
    "../../../lib/soci/src/core/session.cpp"
    "../../../lib/soci/src/core/soci-simple.cpp"
    "../../../lib/soci/src/core/statement.cpp"
    "../../../lib/soci/src/core/transaction.cpp"
    "../../../lib/soci/src/core/use-type.cpp"
    "../../../lib/soci/src/core/values.cpp"
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
set(ROOT_NAMESPACE libsoci)

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
            /MACHINE:X64
        >
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()

