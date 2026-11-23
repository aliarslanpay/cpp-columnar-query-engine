include(FetchContent)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# SHA256 computed from the downloaded release archive (885,595 bytes).
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(googletest)

add_executable(cqe_tests
  test_block_layout.cpp
  test_block_skip.cpp
  test_column_store.cpp
  test_dataset.cpp
  test_equivalence.cpp
  test_fixtures.cpp
  test_row_store.cpp
  test_sum_bound.cpp
  test_zone_map.cpp
  test_zone_map_scan.cpp)
target_link_libraries(cqe_tests PRIVATE cqe cqe_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cqe_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES)
