#
# Compress the setup page and hold it to §9.2's budget.
#
#   cmake -DSOURCE=<in> -DOUTPUT=<out.gz> -DBUDGET=<bytes> -P gzip.cmake
#
# A CMake script rather than a shell one-liner because it runs on whatever
# machine builds the firmware, and a redirection inside add_custom_command needs
# a shell that Windows does not have to have. `-n` keeps the name and the
# timestamp out of the member header, so the same page produces the same bytes on
# every machine and a rebuild does not churn the image for no reason.
#
# The budget is a build failure and not a warning. §9.2 caps the page at 24 KB
# gzipped and the number is easy to walk past: this page is the one thing that
# has to work on a device with nothing else on it, and the flash it competes with
# is #21's fonts and #31's editor bundle. A line in a build log is not a limit.
#
cmake_minimum_required(VERSION 3.16)

foreach(required SOURCE OUTPUT BUDGET)
    if(NOT ${required})
        message(FATAL_ERROR "gzip.cmake: ${required} must be set")
    endif()
endforeach()

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

# Reported on every build that touches the page, because §55 asks for the number
# either way and the only reliable way to have it is to print it.
message(STATUS "Slate setup page: ${packed} B gzipped from ${plain} B, "
               "${percent} % of the ${BUDGET} B budget")

if(packed GREATER BUDGET)
    message(FATAL_ERROR
            "the setup page is ${packed} B gzipped, over §9.2's ${BUDGET} B budget")
endif()
