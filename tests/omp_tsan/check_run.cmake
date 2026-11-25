# Runs PROGRAM and checks both its exit code and its output (stdout and stderr together).
#
#   cmake -DPROGRAM=<path> -DEXPECT_EXIT=<code>
#         [-DREQUIRE_1=<regex>] [-DREQUIRE_2=<regex>] [-DFORBID=<regex>] -P check_run.cmake
#
# The check passes only if the exit code equals EXPECT_EXIT, every REQUIRE_n regex matches and
# FORBID does not match. The program's output is always printed, so the ctest log records it.

if(NOT DEFINED PROGRAM OR NOT DEFINED EXPECT_EXIT)
  message(FATAL_ERROR "check_run.cmake: PROGRAM and EXPECT_EXIT are required")
endif()

execute_process(
  COMMAND "${PROGRAM}"
  RESULT_VARIABLE exit_code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
set(all "${out}${err}")
message("---- output of ${PROGRAM} (exit ${exit_code}) ----\n${all}---- end of output ----")

set(failures "")
if(NOT exit_code STREQUAL EXPECT_EXIT)
  string(APPEND failures "exit code ${exit_code}, expected ${EXPECT_EXIT}; ")
endif()
foreach(var REQUIRE_1 REQUIRE_2)
  if(DEFINED ${var} AND NOT all MATCHES "${${var}}")
    string(APPEND failures "required output not found: '${${var}}'; ")
  endif()
endforeach()
if(DEFINED FORBID AND all MATCHES "${FORBID}")
  string(APPEND failures "forbidden output found: '${FORBID}'; ")
endif()

if(failures)
  message(FATAL_ERROR "check failed: ${failures}")
endif()
message("check passed")
