# ThreadSanitizer + OpenMP + Archer control programs. Built only with CQE_OMP_TSAN_CHECKS=ON,
# which the tsan preset sets. They establish that, in this toolchain, TSan reports a real race in
# OpenMP code and reports nothing for correctly synchronized OpenMP code.

foreach(name IN ITEMS race_free_partials race_free_barrier race_free_reduction racy_counter)
  add_executable(omp_tsan_${name} ${name}.cpp)
  target_link_libraries(omp_tsan_${name} PRIVATE OpenMP::OpenMP_CXX cqe_warnings)
endforeach()

# Every check runs with the same settings; only ARCHER_OPTIONS differs for the activation check.
set(cqe_omp_tsan_env
  "TSAN_OPTIONS=halt_on_error=1:exitcode=66:ignore_noninstrumented_modules=1"
  "OMP_TOOL=enabled")
set(cqe_two_or_more_threads "threads=([2-9]|[1-9][0-9]+)")

function(cqe_omp_tsan_check test_name program)
  cmake_parse_arguments(ARG "" "EXPECT_EXIT;REQUIRE_1;REQUIRE_2;FORBID" "EXTRA_ENV" ${ARGN})
  set(defines -DPROGRAM=$<TARGET_FILE:omp_tsan_${program}> -DEXPECT_EXIT=${ARG_EXPECT_EXIT})
  foreach(key IN ITEMS REQUIRE_1 REQUIRE_2 FORBID)
    if(DEFINED ARG_${key})
      list(APPEND defines "-D${key}=${ARG_${key}}")
    endif()
  endforeach()
  set(env ${cqe_omp_tsan_env})
  if(ARG_EXTRA_ENV)
    list(APPEND env ${ARG_EXTRA_ENV})
  endif()
  add_test(NAME omp_tsan.${test_name}
           COMMAND ${CMAKE_COMMAND} ${defines} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_run.cmake)
  set_tests_properties(omp_tsan.${test_name} PROPERTIES ENVIRONMENT "${env}" LABELS omp_tsan)
endfunction()

# Race-free controls: exit 0, at least two threads, and no ThreadSanitizer output at all.
foreach(name IN ITEMS race_free_partials race_free_barrier race_free_reduction)
  cqe_omp_tsan_check(${name} ${name}
    EXPECT_EXIT 0
    REQUIRE_1 "${name}: OK"
    REQUIRE_2 "${cqe_two_or_more_threads}"
    FORBID "ThreadSanitizer")
endforeach()

# Real race: TSan's exit code and a data-race report must both appear, with at least two threads.
cqe_omp_tsan_check(racy_counter_is_reported racy_counter
  EXPECT_EXIT 66
  REQUIRE_1 "WARNING: ThreadSanitizer: data race"
  REQUIRE_2 "${cqe_two_or_more_threads}")

# Archer is active: its banner appears, the program exits 0 and no sanitizer report is printed.
cqe_omp_tsan_check(archer_active race_free_partials
  EXPECT_EXIT 0
  REQUIRE_1 "Archer detected OpenMP application with TSan, supplying OpenMP synchronization semantics"
  REQUIRE_2 "${cqe_two_or_more_threads}"
  FORBID "ThreadSanitizer|Sanitizer: |runtime error"
  EXTRA_ENV "ARCHER_OPTIONS=verbose=1")
