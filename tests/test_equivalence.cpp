// Random equivalence: both full scans must equal the reference over many seeds, sizes,
// orderings, distributions and predicates.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "cqe/column_store.hpp"
#include "cqe/dataset.hpp"
#include "cqe/executor.hpp"
#include "cqe/reference.hpp"
#include "cqe/row_store.hpp"
#include "test_util.hpp"

namespace cqe {
namespace {

using test::kInt32Max;
using test::kInt32Min;
using test::kInt64Max;
using test::kInt64Min;

using Param =std::tuple<std::uint64_t, std::size_t, Ordering, DeviceDistribution>;

// The number of devices varies with the seed, so equality matches range from every row
// (one device) to sparse (1000 devices).
constexpr std::array<std::int32_t, 4> kDeviceCounts = {1, 7, 100, 1000};
constexpr std::size_t kQueriesPerDataset = 200;

class FullScanEquivalence : public ::testing::TestWithParam<Param> {};

TEST_P(FullScanEquivalence, BothStoresMatchTheReference) {
  const auto [seed, row_count, ordering, distribution] = GetParam();
  GeneratorConfig config;
  config.row_count = row_count;
  config.seed = seed;
  config.device_distribution = distribution;
  config.device_count = kDeviceCounts[seed % kDeviceCounts.size()];

  const std::vector<Event> rows = generate_events(config, ordering);
  const RowStore row_store{rows};
  const ColumnStore column_store{rows};
  const std::vector<Query> queries =
      test::random_queries(rows, config.device_count, seed * 1'000'003 + row_count, kQueriesPerDataset);

  std::size_t empty_results = 0;
  std::size_t non_empty_results = 0;
  for (const Query& query : queries) {
    const QueryResult expected = reference_evaluate(rows, query);
    ASSERT_EQ(full_scan(row_store, query), expected) << "row store, " << ::testing::PrintToString(query);
    ASSERT_EQ(full_scan(column_store, query), expected)
        << "column store, " << ::testing::PrintToString(query);
    ++(expected.count == 0 ? empty_results : non_empty_results);
  }
  // Guard against a degenerate predicate mix that would make the comparison vacuous.
  EXPECT_GT(empty_results, 0U);
  if (row_count > 0) {
    EXPECT_GT(non_empty_results, 0U);
  }
}

// Regression: random_queries() once computed min_ts - 100, max_ts + 100,
// max_ts + 1 and min_ts - 1000 without range checks, which overflows int64 when the data holds
// INT64_MIN or INT64_MAX (reported by UBSan). Tables whose timestamps touch one or both int64
// limits must produce valid queries, and both stores must still match the reference on them.
TEST(RandomQueries, TimestampsAtTheInt64LimitsDoNotOverflow) {
  const std::vector<std::vector<Event>> tables = {
      // Both limits: no range outside [min_ts, max_ts] is representable.
      {{kInt64Min, kInt32Min, 1}, {kInt64Max, kInt32Max, 2}, {0, 0, -4}, {kInt64Min, 3, 8}},
      // Only the upper limit: outside-data ranges must come from below.
      {{-5, 1, 1}, {kInt64Max, 2, 2}, {kInt64Max - 1, 1, -3}},
      // Only the lower limit: outside-data ranges must come from above.
      {{kInt64Min, 1, 1}, {7, 2, 2}, {kInt64Min + 1, 0, -3}},
      // Close to, but not at, both limits: the outer endpoints saturate.
      {{kInt64Min + 3, 0, 1}, {kInt64Max - 3, 1, 2}},
  };
  for (std::size_t t = 0; t < tables.size(); ++t) {
    const std::vector<Event>& rows = tables[t];
    const RowStore row_store{rows};
    const ColumnStore column_store{rows};
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
      for (const Query& query : test::random_queries(rows, 4, seed, 200)) {
        const QueryResult expected = reference_evaluate(rows, query);
        ASSERT_EQ(full_scan(row_store, query), expected)
            << "table " << t << ", row store, " << ::testing::PrintToString(query);
        ASSERT_EQ(full_scan(column_store, query), expected)
            << "table " << t << ", column store, " << ::testing::PrintToString(query);
      }
    }
  }
}

TEST(RandomQueries, SaturatingAddClampsAtTheInt64Limits) {
  EXPECT_EQ(test::saturating_add(5, -3), 2);
  EXPECT_EQ(test::saturating_add(kInt64Max, 100), kInt64Max);
  EXPECT_EQ(test::saturating_add(kInt64Max - 50, 100), kInt64Max);
  EXPECT_EQ(test::saturating_add(kInt64Max - 100, 100), kInt64Max);
  EXPECT_EQ(test::saturating_add(kInt64Min, -100), kInt64Min);
  EXPECT_EQ(test::saturating_add(kInt64Min + 50, -100), kInt64Min);
  EXPECT_EQ(test::saturating_add(kInt64Max, kInt64Min), -1);
}

TEST(RandomQueries, OutsideDataRangeIsNonEmptyAndDisjointFromTheData) {
  struct Case {
    std::int64_t min_ts;
    std::int64_t max_ts;
    bool prefer_above;
    std::optional<std::pair<std::int64_t, std::int64_t>> expected;
  };
  const std::vector<Case> cases = {
      {0, 10, true, std::pair{11, 1010}},
      {0, 10, false, std::pair{-1000, -1}},
      {0, kInt64Max, true, std::pair{-1000, -1}},  // nothing above: falls back below
      {kInt64Min, 0, false, std::pair{1, 1000}},   // nothing below: falls back above
      {kInt64Max - 5, kInt64Max - 1, true, std::pair{kInt64Max, kInt64Max}},  // saturates
      {kInt64Min + 1, kInt64Min + 5, false, std::pair{kInt64Min, kInt64Min}},  // saturates
      {kInt64Min, kInt64Max, true, std::nullopt},   // no representable range outside the data
      {kInt64Min, kInt64Max, false, std::nullopt},
  };
  for (const Case& c : cases) {
    const std::optional<TimestampRange> got =
        test::outside_data_range(c.min_ts, c.max_ts, c.prefer_above);
    ASSERT_EQ(got.has_value(), c.expected.has_value()) << c.min_ts << " " << c.max_ts;
    if (!got) {
      continue;
    }
    EXPECT_EQ(got->lo, c.expected->first) << c.min_ts << " " << c.max_ts;
    EXPECT_EQ(got->hi, c.expected->second) << c.min_ts << " " << c.max_ts;
    EXPECT_LE(got->lo, got->hi);
    EXPECT_TRUE(got->lo > c.max_ts || got->hi < c.min_ts);
  }
}

// A named function rather than a lambda: commas in a structured binding would split the macro
// arguments of INSTANTIATE_TEST_SUITE_P.
std::string param_name(const ::testing::TestParamInfo<Param>& info) {
  const auto& [seed, row_count, ordering, distribution] = info.param;
  return "seed" + std::to_string(seed) + "_rows" + std::to_string(row_count) + "_" +
         std::string(to_string(ordering)) + "_" + std::string(to_string(distribution));
}

INSTANTIATE_TEST_SUITE_P(
    Generated, FullScanEquivalence,
    ::testing::Combine(::testing::Values<std::uint64_t>(1, 2, 3, 4, 5),
                       ::testing::Values<std::size_t>(0, 1, 1023, 1024, 1025, 10000),
                       ::testing::Values(Ordering::TimeSorted, Ordering::Shuffled,
                                         Ordering::DeviceSorted),
                       ::testing::Values(DeviceDistribution::Uniform, DeviceDistribution::Zipf)),
    param_name);

}  // namespace
}  // namespace cqe
