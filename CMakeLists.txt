cmake_minimum_required(VERSION 3.28)

project(columnar_query_prototype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules are used. Without this, CMake 3.28 scans every C++20 source for module
# dependencies, which needs clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(CQE_BUILD_TESTS "Build the test suite" ON)
option(CQE_BUILD_CALIBRATION "Build the scan calibration microbenchmark" OFF)
option(CQE_BUILD_APPEND_BENCHMARK "Build the append maintenance benchmark" OFF)
option(CQE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in project targets" OFF)
option(CQE_OPENMP "Build parallel execution with OpenMP" ON)
option(CQE_OMP_TSAN_CHECKS
  "Build the ThreadSanitizer + OpenMP control programs (for the tsan preset)" OFF)

if(CQE_OPENMP)
  find_package(OpenMP REQUIRED COMPONENTS CXX)
endif()
if(CQE_OMP_TSAN_CHECKS AND NOT CQE_OPENMP)
  message(FATAL_ERROR "CQE_OMP_TSAN_CHECKS requires CQE_OPENMP")
endif()

# Warning flags for project targets only (not for third-party code such as GoogleTest).
add_library(cqe_warnings INTERFACE)
target_compile_options(cqe_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion
  $<$<BOOL:${CQE_WARNINGS_AS_ERRORS}>:-Werror>)

add_library(cqe STATIC
  src/block_index.cpp
  src/block_layout.cpp
  src/block_skip.cpp
  src/column_store.cpp
  src/dataset.cpp
  src/executor.cpp
  src/kernels.cpp
  src/parallel_scan.cpp
  src/reference.cpp
  src/row_store.cpp
  src/strategy.cpp
  src/sum_bound.cpp
  src/zone_map.cpp
  src/zone_mapped_table.cpp)
target_include_directories(cqe PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(cqe PUBLIC cxx_std_20)
target_link_libraries(cqe PRIVATE cqe_warnings)
if(CQE_OPENMP)
  target_link_libraries(cqe PRIVATE OpenMP::OpenMP_CXX)
endif()

if(CQE_BUILD_CALIBRATION)
  add_executable(cqe_calibrate bench/calibrate.cpp)
  target_link_libraries(cqe_calibrate PRIVATE cqe cqe_warnings)
endif()

if(CQE_BUILD_APPEND_BENCHMARK)
  add_executable(cqe_bench_append bench/bench_append.cpp)
  target_link_libraries(cqe_bench_append PRIVATE cqe cqe_warnings)
endif()

if(CQE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  if(CQE_OMP_TSAN_CHECKS)
    add_subdirectory(tests/omp_tsan)
  endif()
endif()
