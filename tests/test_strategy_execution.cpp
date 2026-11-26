#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <variant>

#include "cqe/dataset.hpp"
#include "cqe/executor.hpp"
#include "cqe/reference.hpp"
#include "test_util.hpp"
#include "zone_test_util.hpp"

namespace cqe {
namespace {

using test::device;
using test::range;
using test::stats;

constexpr std::array<Event, 10> kRows{{
    {0, 7, 5}, {1, 7, -9}, {2, 3, 20}, {3, 7, 10}, {4, 8, 4},
    {5, 8, 5}, {6, 4, 1}, {7, 7, -3}, {8, 9, 8}, {9, -1, -11},
}};

TEST(IndexScan, AppliesExactFilterForCommonAndRareDevices) {
  const IndexedTable table{ZoneMappedTable{kRows, 3}};
  for (const auto mode : {ScanMode::Fused, ScanMode::SelectionVector}) {
    for (const int threads : {1, 2, 4, 8}) {
      const ScanOptions options{mode, threads};
      const Execution common = index_scan(table, DeviceEquals{7}, options);
      const Execution rare = index_scan(table, DeviceEquals{-1}, options);
      EXPECT_EQ(common.result, reference_evaluate(kRows, device(7)));
      EXPECT_EQ(common.stats, stats(4, 3, 1, 9, 4));
      EXPECT_EQ(rare.result, reference_evaluate(kRows, device(-1)));
      EXPECT_EQ(rare.stats, stats(4, 1, 3, 1, 1));
    }
  }
}

TEST(IndexScan, EmptyTableAndAbsentDevice) {
  const IndexedTable empty{ZoneMappedTable{{}, 3}};
  const IndexedTable table{ZoneMappedTable{kRows, 3}};
  EXPECT_EQ(index_scan(empty, DeviceEquals{7}), Execution{});
  EXPECT_EQ(index_scan(table, DeviceEquals{6}), (Execution{{}, stats(4, 0, 4, 0, 0)}));
}

TEST(IndexScan, MatchesReferenceAcrossOrderingsAndDistributions) {
  GeneratorConfig config;
  config.row_count = 1'031;
  config.device_count = 11;
  for (const auto distribution : {DeviceDistribution::Uniform, DeviceDistribution::Zipf}) {
    config.device_distribution = distribution;
    for (const auto ordering : {Ordering::TimeSorted, Ordering::Shuffled, Ordering::DeviceSorted}) {
      const auto rows = generate_events(config, ordering);
      const IndexedTable table{ZoneMappedTable{rows, 128}};
      for (std::int32_t id = -1; id <= config.device_count; ++id) {
        ASSERT_EQ(index_scan(table, DeviceEquals{id}).result, reference_evaluate(rows, device(id)))
            << to_string(distribution) << ", " << to_string(ordering) << ", device=" << id;
      }
    }
  }
}

TEST(FullScanWithStats, ExaminesEveryRowAndBlockForBothPredicates) {
  const ZoneMappedTable table{kRows, 3};
  for (const Query query : {device(7), range(8, 9), range(9, 8)}) {
    const QueryResult expected = reference_evaluate(kRows, query);
    for (const auto mode : {ScanMode::Fused, ScanMode::SelectionVector}) {
      for (const int threads : {1, 4}) {
        const Execution execution = full_scan(table, query, ScanOptions{mode, threads});
        EXPECT_EQ(execution.result, expected);
        EXPECT_EQ(execution.stats, stats(4, 4, 0, 10, static_cast<std::uint64_t>(expected.count)));
      }
    }
  }
}

TEST(FullScanWithStats, EmptyTableHasZeroCounters) {
  const ZoneMappedTable table{{}, 3};
  EXPECT_EQ(full_scan(table, device(7)), Execution{});
  EXPECT_EQ(full_scan(table, device(7), ScanOptions{ScanMode::SelectionVector, 4}), Execution{});
}

TEST(StrategyExecution, RejectsInvalidOptionsEvenOnEmptyTables) {
  const IndexedTable table{ZoneMappedTable{{}, 3}};
  EXPECT_THROW((void)index_scan(table, DeviceEquals{7}, ScanOptions{ScanMode::Fused, 0}),
               std::invalid_argument);
  EXPECT_THROW((void)full_scan(table.table(), device(7), ScanOptions{ScanMode::Fused, 0}),
               std::invalid_argument);
  const ScanOptions invalid_mode{static_cast<ScanMode>(-1), 1};
  EXPECT_THROW((void)index_scan(table, DeviceEquals{7}, invalid_mode), std::invalid_argument);
  EXPECT_THROW((void)full_scan(table.table(), device(7), invalid_mode), std::invalid_argument);
  EXPECT_THROW((void)auto_scan(table, device(7), 0), std::invalid_argument);
}

TEST(AutoScan, UsesZoneMapForAnEmptyRange) {
  const IndexedTable table{ZoneMappedTable{kRows, 3}};
  const AutoExecution answer = auto_scan(table, range(9, 8), 4);
  EXPECT_EQ(answer.plan.strategy, ScanStrategy::ZoneMap);
  EXPECT_EQ(answer.plan.threads, 1);
  EXPECT_EQ(answer.execution, (Execution{{}, stats(4, 0, 4, 0, 0)}));
}

TEST(AutoScan, UsesIndexWhenAnAbsentDeviceIsInsideAZone) {
  const IndexedTable table{ZoneMappedTable{kRows, 3}};
  const Query query = device(6);
  ASSERT_GT(zone_map_scan(table.table(), query).stats.blocks_scanned, 0U);
  const AutoExecution answer = auto_scan(table, query, 4);
  EXPECT_EQ(answer.plan.strategy, ScanStrategy::Index);
  EXPECT_EQ(answer.plan.threads, 1);
  EXPECT_EQ(answer.execution, (Execution{{}, stats(4, 0, 4, 0, 0)}));
}

TEST(AutoScan, MatchesReferenceAndReportsTheChosenScanStatistics) {
  GeneratorConfig config;
  config.device_count = 11;
  for (const std::size_t row_count : std::array<std::size_t, 3>{0, 1, 4'099}) {
    config.row_count = row_count;
    const auto rows = generate_events(config, Ordering::DeviceSorted);
    const IndexedTable table{ZoneMappedTable{rows, 128}};
    const auto queries = test::random_queries(rows, config.device_count, 71, 30);
    for (const Query& query : queries) {
      for (const int budget : {1, 4}) {
        const AutoExecution answer = auto_scan(table, query, budget);
        ASSERT_EQ(answer.execution.result, reference_evaluate(rows, query))
            << ::testing::PrintToString(query);
        EXPECT_GE(answer.plan.threads, 1);
        EXPECT_LE(answer.plan.threads, budget);
        EXPECT_EQ(answer.execution.stats.blocks_total, table.table().zone_map().layout().block_count());
        const ScanOptions options{ScanMode::Fused, answer.plan.threads};
        switch (answer.plan.strategy) {
          case ScanStrategy::Full:
            EXPECT_EQ(answer.execution, full_scan(table.table(), query, options));
            break;
          case ScanStrategy::ZoneMap:
            EXPECT_EQ(answer.execution, zone_map_scan(table.table(), query, options));
            break;
          case ScanStrategy::Index:
            ASSERT_TRUE(std::holds_alternative<DeviceEquals>(query.predicate));
            EXPECT_EQ(answer.execution, index_scan(table, std::get<DeviceEquals>(query.predicate),
                                                   options));
            break;
        }
      }
    }
  }
}

}  // namespace
}  // namespace cqe
