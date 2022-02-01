set(PROJECT_NAME libmedida)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/libmedida/src/medida/buckets.h"
    "../../../lib/libmedida/src/medida/counter.h"
    "../../../lib/libmedida/src/medida/histogram.h"
    "../../../lib/libmedida/src/medida/medida.h"
    "../../../lib/libmedida/src/medida/meter.h"
    "../../../lib/libmedida/src/medida/metered_interface.h"
    "../../../lib/libmedida/src/medida/metric_interface.h"
    "../../../lib/libmedida/src/medida/metric_name.h"
    "../../../lib/libmedida/src/medida/metric_processor.h"
    "../../../lib/libmedida/src/medida/metrics_registry.h"
    "../../../lib/libmedida/src/medida/reporting/abstract_polling_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/console_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/json_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/util.h"
    "../../../lib/libmedida/src/medida/sampling_interface.h"
    "../../../lib/libmedida/src/medida/stats/ewma.h"
    "../../../lib/libmedida/src/medida/stats/exp_decay_sample.h"
    "../../../lib/libmedida/src/medida/stats/sample.h"
    "../../../lib/libmedida/src/medida/stats/snapshot.h"
    "../../../lib/libmedida/src/medida/stats/uniform_sample.h"
    "../../../lib/libmedida/src/medida/summarizable_interface.h"
    "../../../lib/libmedida/src/medida/timer.h"
    "../../../lib/libmedida/src/medida/timer_context.h"
    "../../../lib/libmedida/src/medida/types.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/libmedida/src/medida/buckets.cc"
    "../../../lib/libmedida/src/medida/counter.cc"
    "../../../lib/libmedida/src/medida/histogram.cc"
    "../../../lib/libmedida/src/medida/meter.cc"
    "../../../lib/libmedida/src/medida/metric_name.cc"
    "../../../lib/libmedida/src/medida/metric_processor.cc"
    "../../../lib/libmedida/src/medida/metrics_registry.cc"
    "../../../lib/libmedida/src/medida/reporting/abstract_polling_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/console_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/json_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/util.cc"
    "../../../lib/libmedida/src/medida/stats/ckms.cc"
    "../../../lib/libmedida/src/medida/stats/ckms.h"
    "../../../lib/libmedida/src/medida/stats/ckms_sample.cc"
    "../../../lib/libmedida/src/medida/stats/ckms_sample.h"
    "../../../lib/libmedida/src/medida/stats/ewma.cc"
    "../../../lib/libmedida/src/medida/stats/exp_decay_sample.cc"
    "../../../lib/libmedida/src/medida/stats/sliding_window_sample.cc"
    "../../../lib/libmedida/src/medida/stats/sliding_window_sample.h"
    "../../../lib/libmedida/src/medida/stats/snapshot.cc"
    "../../../lib/libmedida/src/medida/stats/uniform_sample.cc"
    "../../../lib/libmedida/src/medida/timer.cc"
    "../../../lib/libmedida/src/medida/timer_context.cc"
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
set(ROOT_NAMESPACE libmedida)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/libmedida/src"
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
            /wd4244
        >
        $<$<CONFIG:DebugNoPostgres>:
            /MP;
            /Gm-;
            /wd4244
        >
        $<$<CONFIG:Release>:
            /Oi;
            /Gy
        >
        /Od;
        /sdl;
        /W3;
        ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
        ${DEFAULT_CXX_EXCEPTION_HANDLING}
    )
    string(CONCAT FILE_CL_OPTIONS
        "$<$<CONFIG:Debug>:"
            "/wd4244;/wd4996"
        ">"
        "$<$<CONFIG:DebugNoPostgres>:"
            "/wd4244;/wd4996"
        ">"
    )
    source_file_compile_options(../../../lib/libmedida/src/medida/reporting/json_reporter.cc ${FILE_CL_OPTIONS})
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()



























set(PROJECT_NAME libmedida)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/libmedida/src/medida/buckets.h"
    "../../../lib/libmedida/src/medida/counter.h"
    "../../../lib/libmedida/src/medida/histogram.h"
    "../../../lib/libmedida/src/medida/medida.h"
    "../../../lib/libmedida/src/medida/meter.h"
    "../../../lib/libmedida/src/medida/metered_interface.h"
    "../../../lib/libmedida/src/medida/metric_interface.h"
    "../../../lib/libmedida/src/medida/metric_name.h"
    "../../../lib/libmedida/src/medida/metric_processor.h"
    "../../../lib/libmedida/src/medida/metrics_registry.h"
    "../../../lib/libmedida/src/medida/reporting/abstract_polling_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/console_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/json_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/util.h"
    "../../../lib/libmedida/src/medida/sampling_interface.h"
    "../../../lib/libmedida/src/medida/stats/ewma.h"
    "../../../lib/libmedida/src/medida/stats/exp_decay_sample.h"
    "../../../lib/libmedida/src/medida/stats/sample.h"
    "../../../lib/libmedida/src/medida/stats/snapshot.h"
    "../../../lib/libmedida/src/medida/stats/uniform_sample.h"
    "../../../lib/libmedida/src/medida/summarizable_interface.h"
    "../../../lib/libmedida/src/medida/timer.h"
    "../../../lib/libmedida/src/medida/timer_context.h"
    "../../../lib/libmedida/src/medida/types.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/libmedida/src/medida/buckets.cc"
    "../../../lib/libmedida/src/medida/counter.cc"
    "../../../lib/libmedida/src/medida/histogram.cc"
    "../../../lib/libmedida/src/medida/meter.cc"
    "../../../lib/libmedida/src/medida/metric_name.cc"
    "../../../lib/libmedida/src/medida/metric_processor.cc"
    "../../../lib/libmedida/src/medida/metrics_registry.cc"
    "../../../lib/libmedida/src/medida/reporting/abstract_polling_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/console_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/json_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/util.cc"
    "../../../lib/libmedida/src/medida/stats/ckms.cc"
    "../../../lib/libmedida/src/medida/stats/ckms.h"
    "../../../lib/libmedida/src/medida/stats/ckms_sample.cc"
    "../../../lib/libmedida/src/medida/stats/ckms_sample.h"
    "../../../lib/libmedida/src/medida/stats/ewma.cc"
    "../../../lib/libmedida/src/medida/stats/exp_decay_sample.cc"
    "../../../lib/libmedida/src/medida/stats/sliding_window_sample.cc"
    "../../../lib/libmedida/src/medida/stats/sliding_window_sample.h"
    "../../../lib/libmedida/src/medida/stats/snapshot.cc"
    "../../../lib/libmedida/src/medida/stats/uniform_sample.cc"
    "../../../lib/libmedida/src/medida/timer.cc"
    "../../../lib/libmedida/src/medida/timer_context.cc"
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
set(ROOT_NAMESPACE libmedida)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/libmedida/src"
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
            /wd4244
        >
        $<$<CONFIG:DebugNoPostgres>:
            /MP;
            /Gm-;
            /wd4244
        >
        $<$<CONFIG:Release>:
            /Oi;
            /Gy
        >
        /Od;
        /sdl;
        /W3;
        ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
        ${DEFAULT_CXX_EXCEPTION_HANDLING}
    )
    string(CONCAT FILE_CL_OPTIONS
        "$<$<CONFIG:Debug>:"
            "/wd4244;/wd4996"
        ">"
        "$<$<CONFIG:DebugNoPostgres>:"
            "/wd4244;/wd4996"
        ">"
    )
    source_file_compile_options(../../../lib/libmedida/src/medida/reporting/json_reporter.cc ${FILE_CL_OPTIONS})
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()



























set(PROJECT_NAME libmedida)

################################################################################
# Source groups
################################################################################
set(Header_Files
    "../../../lib/libmedida/src/medida/buckets.h"
    "../../../lib/libmedida/src/medida/counter.h"
    "../../../lib/libmedida/src/medida/histogram.h"
    "../../../lib/libmedida/src/medida/medida.h"
    "../../../lib/libmedida/src/medida/meter.h"
    "../../../lib/libmedida/src/medida/metered_interface.h"
    "../../../lib/libmedida/src/medida/metric_interface.h"
    "../../../lib/libmedida/src/medida/metric_name.h"
    "../../../lib/libmedida/src/medida/metric_processor.h"
    "../../../lib/libmedida/src/medida/metrics_registry.h"
    "../../../lib/libmedida/src/medida/reporting/abstract_polling_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/console_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/json_reporter.h"
    "../../../lib/libmedida/src/medida/reporting/util.h"
    "../../../lib/libmedida/src/medida/sampling_interface.h"
    "../../../lib/libmedida/src/medida/stats/ewma.h"
    "../../../lib/libmedida/src/medida/stats/exp_decay_sample.h"
    "../../../lib/libmedida/src/medida/stats/sample.h"
    "../../../lib/libmedida/src/medida/stats/snapshot.h"
    "../../../lib/libmedida/src/medida/stats/uniform_sample.h"
    "../../../lib/libmedida/src/medida/summarizable_interface.h"
    "../../../lib/libmedida/src/medida/timer.h"
    "../../../lib/libmedida/src/medida/timer_context.h"
    "../../../lib/libmedida/src/medida/types.h"
)
source_group("Header Files" FILES ${Header_Files})

set(Source_Files
    "../../../lib/libmedida/src/medida/buckets.cc"
    "../../../lib/libmedida/src/medida/counter.cc"
    "../../../lib/libmedida/src/medida/histogram.cc"
    "../../../lib/libmedida/src/medida/meter.cc"
    "../../../lib/libmedida/src/medida/metric_name.cc"
    "../../../lib/libmedida/src/medida/metric_processor.cc"
    "../../../lib/libmedida/src/medida/metrics_registry.cc"
    "../../../lib/libmedida/src/medida/reporting/abstract_polling_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/console_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/json_reporter.cc"
    "../../../lib/libmedida/src/medida/reporting/util.cc"
    "../../../lib/libmedida/src/medida/stats/ckms.cc"
    "../../../lib/libmedida/src/medida/stats/ckms.h"
    "../../../lib/libmedida/src/medida/stats/ckms_sample.cc"
    "../../../lib/libmedida/src/medida/stats/ckms_sample.h"
    "../../../lib/libmedida/src/medida/stats/ewma.cc"
    "../../../lib/libmedida/src/medida/stats/exp_decay_sample.cc"
    "../../../lib/libmedida/src/medida/stats/sliding_window_sample.cc"
    "../../../lib/libmedida/src/medida/stats/sliding_window_sample.h"
    "../../../lib/libmedida/src/medida/stats/snapshot.cc"
    "../../../lib/libmedida/src/medida/stats/uniform_sample.cc"
    "../../../lib/libmedida/src/medida/timer.cc"
    "../../../lib/libmedida/src/medida/timer_context.cc"
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
set(ROOT_NAMESPACE libmedida)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/libmedida/src"
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
            /wd4244
        >
        $<$<CONFIG:DebugNoPostgres>:
            /MP;
            /Gm-;
            /wd4244
        >
        $<$<CONFIG:Release>:
            /Oi;
            /Gy
        >
        /Od;
        /sdl;
        /W3;
        ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
        ${DEFAULT_CXX_EXCEPTION_HANDLING}
    )
    string(CONCAT FILE_CL_OPTIONS
        "$<$<CONFIG:Debug>:"
            "/wd4244;/wd4996"
        ">"
        "$<$<CONFIG:DebugNoPostgres>:"
            "/wd4244;/wd4996"
        ">"
    )
    source_file_compile_options(../../../lib/libmedida/src/medida/reporting/json_reporter.cc ${FILE_CL_OPTIONS})
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()

