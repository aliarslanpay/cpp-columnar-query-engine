// Zone-map scan: fixtures with hand-computed results and statistics, the direct false-negative
// check, and random equivalence against the reference and the full scan.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
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
using test::kInt32Min;
using test::kInt64Max;
using test::kInt64Min;
using test::range;
using test::stats;

// Runs a zone-map scan and checks everything that does not depend on the fixture's expected
// values: equality with the reference and with the full scan, the statistics invariants, and the
// direct false-negative check.
Execution run_checked(const std::vector<Event>& rows, const ZoneMappedTable& table,
                      const Query& query) {
  const Execution execution = zone_map_scan(table, query);
  EXPECT_EQ(execution.result, reference_evaluate(rows, query)) << ::testing::PrintToString(query);
  EXPECT_EQ(execution.result, full_scan(table.store(), query)) << ::testing::PrintToString(query);
  test::expect_stats_invariants(execution, table, query);
  test::expect_no_false_negatives(rows, table, query);
  return execution;
}

// Ten rows, capacity 4: blocks B0 = rows 0-3, B1 = rows 4-7, and the partial block B2 = rows 8-9.
// Row i has timestamp 10 + i and amount 2^i, so every sum names its rows.
//   B0: timestamp [10, 13], device_id [1, 2]
//   B1: timestamp [14, 17], device_id [3, 4]
//   B2: timestamp [18, 19], device_id [5, 5]
std::vector<Event> ten_rows() {
  return {{10, 1, 1},  {11, 1, 2},  {12, 2, 4},  {13, 2, 8},   {14, 3, 16},
          {15, 3, 32}, {16, 4, 64}, {17, 4, 128}, {18, 5, 256}, {19, 5, 512}};
}

class TenRowFixture : public ::testing::Test {
 protected:
  Execution run(const Query& query) const { return run_checked(rows_, table_, query); }

  const std::vector<Event> rows_ = ten_rows();
  const ZoneMappedTable table_{rows_, 4};
};

TEST_F(TenRowFixture, MatchesAtBlockBoundaries) {
  // Last row of B0, first row of B1, and a range across the B0/B1 boundary.
  EXPECT_EQ(run(range(13, 13)), (Execution{{1, 8}, stats(3, 1, 2, 4, 1)}));
  EXPECT_EQ(run(range(14, 14)), (Execution{{1, 16}, stats(3, 1, 2, 4, 1)}));
  EXPECT_EQ(run(range(13, 14)), (Execution{{2, 24}, stats(3, 2, 1, 8, 2)}));
  // Across the B1/B2 boundary: rows 7 and 8.
  EXPECT_EQ(run(range(17, 18)), (Execution{{2, 384}, stats(3, 2, 1, 6, 2)}));
}

TEST_F(TenRowFixture, PartialLastBlockCountsOnlyItsOwnRows) {
  // Only B2 is scanned, and it has 2 rows, not 4.
  EXPECT_EQ(run(range(18, 19)), (Execution{{2, 768}, stats(3, 1, 2, 2, 2)}));
  EXPECT_EQ(run(range(19, 100)), (Execution{{1, 512}, stats(3, 1, 2, 2, 1)}));
  EXPECT_EQ(run(device(5)), (Execution{{2, 768}, stats(3, 1, 2, 2, 2)}));
}

TEST_F(TenRowFixture, ValuesEqualToABlockMinOrMaxKeepTheBlock) {
  EXPECT_EQ(run(range(17, 17)), (Execution{{1, 128}, stats(3, 1, 2, 4, 1)}));  // B1 max
  EXPECT_EQ(run(range(5, 10)), (Execution{{1, 1}, stats(3, 1, 2, 4, 1)}));     // hi == B0 min
  EXPECT_EQ(run(range(19, 25)), (Execution{{1, 512}, stats(3, 1, 2, 2, 1)}));  // lo == B2 max
  // hi == B2 min and B1 inside: rows 6, 7 and 8; B0 is skipped.
  EXPECT_EQ(run(range(16, 18)), (Execution{{3, 448}, stats(3, 2, 1, 6, 3)}));
  EXPECT_EQ(run(device(1)), (Execution{{2, 3}, stats(3, 1, 2, 4, 2)}));   // B0 min
  EXPECT_EQ(run(device(2)), (Execution{{2, 12}, stats(3, 1, 2, 4, 2)}));  // B0 max
  EXPECT_EQ(run(device(3)), (Execution{{2, 48}, stats(3, 1, 2, 4, 2)}));  // B1 min
}

TEST_F(TenRowFixture, AllBlocksSkipped) {
  const Execution none{{0, 0}, stats(3, 0, 3, 0, 0)};
  EXPECT_EQ(run(range(20, 30)), none);  // above the data
  EXPECT_EQ(run(range(0, 9)), none);    // below the data
  EXPECT_EQ(run(range(15, 14)), none);  // inverted, inside B1's zone
  EXPECT_EQ(run(range(19, 10)), none);  // inverted, over the whole data
  EXPECT_EQ(run(range(kInt64Max, kInt64Min)), none);
  EXPECT_EQ(run(device(0)), none);
  EXPECT_EQ(run(device(6)), none);
  EXPECT_EQ(run(device(kInt32Min)), none);
}

TEST_F(TenRowFixture, NoBlockSkipped) {
  EXPECT_EQ(run(range(kInt64Min, kInt64Max)), (Execution{{10, 1023}, stats(3, 3, 0, 10, 10)}));
  EXPECT_EQ(run(range(10, 19)), (Execution{{10, 1023}, stats(3, 3, 0, 10, 10)}));
  // B0 and B2 are kept by a single shared endpoint each: rows 3 to 8.
  EXPECT_EQ(run(range(13, 18)), (Execution{{6, 504}, stats(3, 3, 0, 10, 6)}));
}

TEST(ZoneMapScan, FalsePositiveBlockIsScannedAndFilteredByRows) {
  // B0 has timestamps {0, 10, 20, 30} (zone [0, 30]) and device ids {0, 9, 0, 9} (zone [0, 9]);
  // B1 has timestamps 40-43 and device id 1.
  const std::vector<Event> rows = {{0, 0, 1},  {10, 9, 2},  {20, 0, 3},  {30, 9, 4},
                                   {40, 1, 5}, {41, 1, 6}, {42, 1, 7}, {43, 1, 8}};
  const ZoneMappedTable table{rows, 4};
  // The zones of B0 overlap these predicates although no row of B0 matches.
  EXPECT_EQ(run_checked(rows, table, range(11, 19)), (Execution{{0, 0}, stats(2, 1, 1, 4, 0)}));
  EXPECT_EQ(run_checked(rows, table, device(5)), (Execution{{0, 0}, stats(2, 1, 1, 4, 0)}));
  // Between the two zones: nothing is scanned.
  EXPECT_EQ(run_checked(rows, table, range(31, 39)), (Execution{{0, 0}, stats(2, 0, 2, 0, 0)}));
}

TEST(ZoneMapScan, EmptyTable) {
  const std::vector<Event> rows;
  const ZoneMappedTable table{rows, 4};
  const Execution nothing{{0, 0}, stats(0, 0, 0, 0, 0)};
  EXPECT_EQ(run_checked(rows, table, range(kInt64Min, kInt64Max)), nothing);
  EXPECT_EQ(run_checked(rows, table, range(5, 1)), nothing);
  EXPECT_EQ(run_checked(rows, table, device(0)), nothing);
}

TEST(ZoneMapScan, SingleRow) {
  const std::vector<Event> rows = {{10, 7, -5}};
  const ZoneMappedTable table{rows};
  EXPECT_EQ(run_checked(rows, table, range(10, 10)), (Execution{{1, -5}, stats(1, 1, 0, 1, 1)}));
  EXPECT_EQ(run_checked(rows, table, range(11, 20)), (Execution{{0, 0}, stats(1, 0, 1, 0, 0)}));
  EXPECT_EQ(run_checked(rows, table, device(7)), (Execution{{1, -5}, stats(1, 1, 0, 1, 1)}));
  EXPECT_EQ(run_checked(rows, table, device(8)), (Execution{{0, 0}, stats(1, 0, 1, 0, 0)}));
}

TEST(ZoneMapScan, CapacityOne) {
  const std::vector<Event> rows = ten_rows();
  const ZoneMappedTable table{rows, 1};
  EXPECT_EQ(run_checked(rows, table, range(13, 14)), (Execution{{2, 24}, stats(10, 2, 8, 2, 2)}));
  EXPECT_EQ(run_checked(rows, table, device(3)), (Execution{{2, 48}, stats(10, 2, 8, 2, 2)}));
}

TEST(ZoneMapScan, CapacityLargerThanTheTable) {
  const std::vector<Event> rows = ten_rows();
  const ZoneMappedTable table{rows, 1024};
  EXPECT_EQ(run_checked(rows, table, range(13, 14)), (Execution{{2, 24}, stats(1, 1, 0, 10, 2)}));
  EXPECT_EQ(run_checked(rows, table, range(20, 30)), (Execution{{0, 0}, stats(1, 0, 1, 0, 0)}));
}

// Each table answers from the zone map of its own rows. (With a store and a zone map passed
// separately, A's zone map would have skipped B's only matching block.)
TEST(ZoneMapScan, EachTableUsesTheZoneMapOfItsOwnRows) {
  const std::vector<Event> rows_a = {{10, 1, 3}};
  const std::vector<Event> rows_b = {{100, 1, 7}};
  const ZoneMappedTable a{rows_a};
  const ZoneMappedTable b{rows_b};
  EXPECT_EQ(run_checked(rows_b, b, range(100, 100)), (Execution{{1, 7}, stats(1, 1, 0, 1, 1)}));
  EXPECT_EQ(run_checked(rows_a, a, range(100, 100)), (Execution{{0, 0}, stats(1, 0, 1, 0, 0)}));
}

TEST(ZoneMapScan, MovedFromTableIsTheEmptyTable) {
  const std::vector<Event> rows = ten_rows();
  ZoneMappedTable source{rows, 4};
  const ZoneMappedTable target{std::move(source)};
  const Query all = range(kInt64Min, kInt64Max);
  EXPECT_EQ(zone_map_scan(target, all), (Execution{{10, 1023}, stats(3, 3, 0, 10, 10)}));
  // Inspecting the moved-from state is the point of this test.
  EXPECT_EQ(zone_map_scan(source, all), (Execution{{0, 0}, stats(0, 0, 0, 0, 0)}));
}

// On time-sorted data a point query touches at most two blocks (a run of equal timestamps can
// cross one block boundary), so pruning must skip most of the ten blocks.
TEST(ZoneMapScan, PrunesTimeSortedDataForPointQueries) {
  GeneratorConfig config;
  config.row_count = 10000;
  config.seed = 21;
  const std::vector<Event> rows = generate_events(config, Ordering::TimeSorted);
  const ZoneMappedTable table{rows, 1024};
  for (const std::size_t i : {std::size_t{0}, std::size_t{1500}, std::size_t{5000}, std::size_t{9999}}) {
    const std::int64_t t = rows[i].timestamp;
    const Execution execution = run_checked(rows, table, range(t, t));
    EXPECT_EQ(execution.stats.blocks_total, 10U);
    EXPECT_GE(execution.stats.blocks_scanned, 1U) << "row " << i;
    EXPECT_LE(execution.stats.blocks_scanned, 2U) << "row " << i;
  }
}

// --- Random equivalence -------------------------------------------------------------------------

using ScanParam = std::tuple<std::uint64_t, std::size_t, Ordering, DeviceDistribution, std::size_t>;

constexpr std::array<std::int32_t, 4> kDeviceCounts = {1, 7, 100, 1000};
constexpr std::size_t kQueriesPerDataset = 200;

class ZoneMapScanEquivalence : public ::testing::TestWithParam<ScanParam> {};

TEST_P(ZoneMapScanEquivalence, MatchesReferenceWithValidStatsAndNoFalseNegatives) {
  const auto [seed, row_count, ordering, distribution, capacity] = GetParam();
  GeneratorConfig config;
  config.row_count = row_count;
  config.seed = seed;
  config.device_distribution = distribution;
  config.device_count = kDeviceCounts[seed % kDeviceCounts.size()];

  const std::vector<Event> rows = generate_events(config, ordering);
  const ZoneMappedTable table{rows, capacity};
  const std::vector<Query> queries = test::random_queries(
      rows, config.device_count, seed * 7'919 + row_count * 31 + capacity, kQueriesPerDataset);

  for (const Query& query : queries) {
    const Execution execution = zone_map_scan(table, query);
    ASSERT_EQ(execution.result, reference_evaluate(rows, query)) << ::testing::PrintToString(query);
    ASSERT_EQ(execution.result, full_scan(table.store(), query)) << ::testing::PrintToString(query);
    test::expect_stats_invariants(execution, table, query);
    test::expect_no_false_negatives(rows, table, query);
    if (HasFailure()) {
      return;  // keep the output short: report the first failing query only
    }
  }
}

std::string scan_param_name(const ::testing::TestParamInfo<ScanParam>& info) {
  const auto& [seed, row_count, ordering, distribution, capacity] = info.param;
  return "seed" + std::to_string(seed) + "_rows" + std::to_string(row_count) + "_cap" +
         std::to_string(capacity) + "_" + std::string(to_string(ordering)) + "_" +
         std::string(to_string(distribution));
}

INSTANTIATE_TEST_SUITE_P(
    Generated, ZoneMapScanEquivalence,
    ::testing::Combine(::testing::Values<std::uint64_t>(1, 2, 3),
                       ::testing::Values<std::size_t>(0, 1, 1023, 1024, 1025, 5000),
                       ::testing::Values(Ordering::TimeSorted, Ordering::Shuffled,
                                         Ordering::DeviceSorted),
                       ::testing::Values(DeviceDistribution::Uniform, DeviceDistribution::Zipf),
                       ::testing::Values<std::size_t>(1, 7, 1024)),
    scan_param_name);

}  // namespace
}  // namespace cqe
