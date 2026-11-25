// Zone-map scan through the ScanOptions overload: every mode and thread count must give the same
// result and statistics as the two-argument serial fused baseline, and equal the reference.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "cqe/dataset.hpp"
#include "cqe/executor.hpp"
#include "cqe/reference.hpp"
#include "cqe/zone_mapped_table.hpp"
#include "test_util.hpp"
#include "zone_test_util.hpp"

namespace cqe {
namespace {

using test::device;
using test::kInt64Max;
using test::kInt64Min;
using test::range;
using test::stats;

constexpr std::array<int, 4> kThreadCounts = {1, 2, 4, 8};

const char* mode_name(ScanMode mode) {
  return mode == ScanMode::Fused ? "fused" : "selection_vector";
}

// Checks one query in every mode and thread count against the baseline and the reference, and
// checks that Fused and SelectionVector agree for each thread count. Returns the baseline.
Execution expect_all_modes_and_threads_agree(const std::vector<Event>& rows,
                                             const ZoneMappedTable& table, const Query& query) {
  const Execution baseline = zone_map_scan(table, query);
  EXPECT_EQ(baseline.result, reference_evaluate(rows, query)) << ::testing::PrintToString(query);
  test::expect_stats_invariants(baseline, table, query);
  test::expect_no_false_negatives(rows, table, query);
  for (const int threads : kThreadCounts) {
    const Execution fused = zone_map_scan(table, query, ScanOptions{ScanMode::Fused, threads});
    const Execution selection =
        zone_map_scan(table, query, ScanOptions{ScanMode::SelectionVector, threads});
    for (const auto& [mode, execution] : {std::pair{ScanMode::Fused, fused},
                                          std::pair{ScanMode::SelectionVector, selection}}) {
      EXPECT_EQ(execution, baseline)
          << mode_name(mode) << ", threads " << threads << ", " << ::testing::PrintToString(query);
    }
    EXPECT_EQ(fused, selection) << "threads " << threads << ", " << ::testing::PrintToString(query);
  }
  return baseline;
}

// Ten rows, capacity 4 (the same data as the zone-map scan fixtures): B0 = rows 0-3, B1 = rows 4-7, B2 = rows 8-9;
// row i has timestamp 10 + i and amount 2^i.
std::vector<Event> ten_rows() {
  return {{10, 1, 1},  {11, 1, 2},  {12, 2, 4},  {13, 2, 8},   {14, 3, 16},
          {15, 3, 32}, {16, 4, 64}, {17, 4, 128}, {18, 5, 256}, {19, 5, 512}};
}

TEST(ParallelZoneMapScan, HandComputedResultsAndStatisticsInEveryModeAndThreadCount) {
  const std::vector<Event> rows = ten_rows();
  const ZoneMappedTable table{rows, 4};
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, range(13, 14)),
            (Execution{{2, 24}, stats(3, 2, 1, 8, 2)}));
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, range(18, 19)),
            (Execution{{2, 768}, stats(3, 1, 2, 2, 2)}));
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, range(13, 18)),
            (Execution{{6, 504}, stats(3, 3, 0, 10, 6)}));
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, range(kInt64Min, kInt64Max)),
            (Execution{{10, 1023}, stats(3, 3, 0, 10, 10)}));
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, device(3)),
            (Execution{{2, 48}, stats(3, 1, 2, 4, 2)}));
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, range(15, 14)),
            (Execution{{0, 0}, stats(3, 0, 3, 0, 0)}));
}

TEST(ParallelZoneMapScan, CapacityOneGivesOneBlockPerRow) {
  const std::vector<Event> rows = ten_rows();
  const ZoneMappedTable table{rows, 1};
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, range(13, 16)),
            (Execution{{4, 120}, stats(10, 4, 6, 4, 4)}));
}

TEST(ParallelZoneMapScan, EmptyTable) {
  const std::vector<Event> rows;
  const ZoneMappedTable table{rows, 4};
  EXPECT_EQ(expect_all_modes_and_threads_agree(rows, table, range(kInt64Min, kInt64Max)),
            (Execution{{0, 0}, stats(0, 0, 0, 0, 0)}));
}

TEST(ParallelZoneMapScan, RejectsInvalidThreadCounts) {
  const std::vector<Event> rows = ten_rows();
  const ZoneMappedTable table{rows, 4};
  const ZoneMappedTable empty{std::vector<Event>{}, 4};
  for (const ScanMode mode : {ScanMode::Fused, ScanMode::SelectionVector}) {
    for (const int threads : {0, -2}) {
      EXPECT_THROW((void)zone_map_scan(table, range(10, 19), ScanOptions{mode, threads}),
                   std::invalid_argument);
      EXPECT_THROW((void)zone_map_scan(empty, range(10, 19), ScanOptions{mode, threads}),
                   std::invalid_argument);
    }
  }
}

// --- Random equivalence -------------------------------------------------------------------------

using ParallelParam =
    std::tuple<std::uint64_t, std::size_t, Ordering, DeviceDistribution, std::size_t>;

constexpr std::array<std::int32_t, 4> kDeviceCounts = {1, 7, 100, 1000};
constexpr std::size_t kQueriesPerDataset = 100;

class ParallelZoneMapScanEquivalence : public ::testing::TestWithParam<ParallelParam> {};

TEST_P(ParallelZoneMapScanEquivalence, EveryModeAndThreadCountMatchesTheBaseline) {
  const auto [seed, row_count, ordering, distribution, capacity] = GetParam();
  GeneratorConfig config;
  config.row_count = row_count;
  config.seed = seed;
  config.device_distribution = distribution;
  config.device_count = kDeviceCounts[seed % kDeviceCounts.size()];

  const std::vector<Event> rows = generate_events(config, ordering);
  const ZoneMappedTable table{rows, capacity};
  const std::vector<Query> queries = test::random_queries(
      rows, config.device_count, seed * 104'729 + row_count * 17 + capacity, kQueriesPerDataset);
  for (const Query& query : queries) {
    (void)expect_all_modes_and_threads_agree(rows, table, query);
    if (HasFailure()) {
      return;  // report the first failing query only
    }
  }
}

std::string parallel_param_name(const ::testing::TestParamInfo<ParallelParam>& info) {
  const auto& [seed, row_count, ordering, distribution, capacity] = info.param;
  return "seed" + std::to_string(seed) + "_rows" + std::to_string(row_count) + "_cap" +
         std::to_string(capacity) + "_" + std::string(to_string(ordering)) + "_" +
         std::string(to_string(distribution));
}

INSTANTIATE_TEST_SUITE_P(
    Generated, ParallelZoneMapScanEquivalence,
    ::testing::Combine(::testing::Values<std::uint64_t>(1, 2),
                       ::testing::Values<std::size_t>(0, 1, 1025, 20000),
                       ::testing::Values(Ordering::TimeSorted, Ordering::Shuffled,
                                         Ordering::DeviceSorted),
                       ::testing::Values(DeviceDistribution::Uniform, DeviceDistribution::Zipf),
                       ::testing::Values<std::size_t>(7, 1024)),
    parallel_param_name);

}  // namespace
}  // namespace cqe
