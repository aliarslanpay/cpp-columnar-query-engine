cmake_minimum_required(VERSION 3.28)

project(columnar_query_prototype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules are used. Without this, CMake 3.28 scans every C++20 source for module
# dependencies, which needs clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(CQE_BUILD_TESTS "Build the test suite" ON)
option(CQE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in project targets" OFF)

# Warning flags for project targets only (not for third-party code such as GoogleTest).
add_library(cqe_warnings INTERFACE)
target_compile_options(cqe_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion
  $<$<BOOL:${CQE_WARNINGS_AS_ERRORS}>:-Werror>)

add_library(cqe STATIC
  src/block_layout.cpp
  src/block_skip.cpp
  src/column_store.cpp
  src/dataset.cpp
  src/executor.cpp
  src/reference.cpp
  src/row_store.cpp
  src/sum_bound.cpp
  src/zone_map.cpp
  src/zone_mapped_table.cpp)
target_include_directories(cqe PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(cqe PUBLIC cxx_std_20)
target_link_libraries(cqe PRIVATE cqe_warnings)

if(CQE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
