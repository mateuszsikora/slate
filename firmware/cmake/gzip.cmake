#
# Compress a static asset and hold it to the budget the design gives it.
#
#   cmake -DSOURCE=<in> -DOUTPUT=<out.gz> -DBUDGET=<bytes> -DLABEL=<what> -P gzip.cmake
#
# Two documents cap a file the firmware serves: §9.2 gives the setup page 24 KB
# and §10 gives the editor bundle 400 KB. Both are compressed the same way and
# both are easy to walk past, so the script is shared rather than copied — the
# second copy is the one that stops being fixed.
#
# A CMake script rather than a shell one-liner because it runs on whatever
# machine builds the firmware, and a redirection inside add_custom_command needs
# a shell that Windows does not have to have. `-n` keeps the name and the
# timestamp out of the member header, so the same input produces the same bytes
# on every machine and a rebuild does not churn the image for no reason.
#
# The budget is a build failure and not a warning. A line in a build log is not
# a limit: the flash these assets compete with is #21's fonts, each other, and
# whatever the next component needs.
#
cmake_minimum_required(VERSION 3.16)

foreach(required SOURCE OUTPUT BUDGET LABEL)
    if(NOT ${required})
        message(FATAL_ERROR "gzip.cmake: ${required} must be set")
    endif()
endforeach()

if(NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "gzip.cmake: ${SOURCE} does not exist")
endif()

find_program(GZIP_EXECUTABLE gzip)
if(NOT GZIP_EXECUTABLE)
    message(FATAL_ERROR "gzip.cmake: gzip was not found on PATH")
endif()

execute_process(
    COMMAND "${GZIP_EXECUTABLE}" -9 -n -c "${SOURCE}"
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "gzip.cmake: compressing ${SOURCE} failed (${result})")
endif()

file(SIZE "${SOURCE}" plain)
file(SIZE "${OUTPUT}" packed)
math(EXPR percent "${packed} * 100 / ${BUDGET}")

# Reported on every build that touches the asset, because the number is asked
# for either way and the only reliable way to have it is to print it.
message(STATUS "Slate ${LABEL}: ${packed} B gzipped from ${plain} B, "
               "${percent} % of the ${BUDGET} B budget")

if(packed GREATER BUDGET)
    message(FATAL_ERROR
            "the ${LABEL} is ${packed} B gzipped, over its ${BUDGET} B budget")
endif()
