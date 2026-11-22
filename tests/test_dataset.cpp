#include "cqe/dataset.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "cqe/column_store.hpp"
#include "cqe/executor.hpp"
#include "cqe/reference.hpp"
#include "cqe/row_store.hpp"
#include "test_util.hpp"

namespace cqe {
namespace {

using test::kInt64Max;
using test::kInt64Min;

constexpr std::array kOrderings = {Ordering::TimeSorted, Ordering::Shuffled, Ordering::DeviceSorted};
constexpr std::array kDistributions = {DeviceDistribution::Uniform, DeviceDistribution::Zipf};

GeneratorConfig config_with(std::size_t row_count,
                            DeviceDistribution distribution = DeviceDistribution::Uniform,
                            std::uint64_t seed = 1) {
  GeneratorConfig config;
  config.row_count = row_count;
  config.device_distribution = distribution;
  config.seed = seed;
  return config;
}

std::vector<std::int64_t> device_counts(const std::vector<Event>& rows, std::int32_t device_count) {
  std::vector<std::int64_t> counts(static_cast<std::size_t>(device_count), 0);
  for (const Event& row : rows) {
    ++counts[static_cast<std::size_t>(row.device_id)];
  }
  return counts;
}

// |observed - expected| <= 6 standard deviations of a binomial count (+1 for rounding).
void expect_count_near(std::int64_t observed, double n, double p, std::int32_t id) {
  const double expected = n * p;
  const double tolerance = 6.0 * std::sqrt(n * p * (1.0 - p)) + 1.0;
  EXPECT_LE(std::abs(static_cast<double>(observed) - expected), tolerance)
      << "device " << id << ": observed " << observed << ", expected " << expected;
}

// --- Shape, ordering and multiset ---------------------------------------------------------------

TEST(Generator, ZeroRowsGiveAnEmptyTable) {
  for (const DeviceDistribution distribution : kDistributions) {
    for (const Ordering ordering : kOrderings) {
      EXPECT_TRUE(generate_events(config_with(0, distribution), ordering).empty())
          << to_string(ordering) << " " << to_string(distribution);
    }
  }
}

TEST(Generator, ProducesExactlyRowCountRows) {
  for (const std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{1023}, std::size_t{1024},
                              std::size_t{1025}, std::size_t{10000}}) {
    for (const Ordering ordering : kOrderings) {
      EXPECT_EQ(generate_events(config_with(n), ordering).size(), n) << to_string(ordering);
    }
  }
}

TEST(Generator, SameConfigGivesIdenticalRows) {
  for (const DeviceDistribution distribution : kDistributions) {
    for (const Ordering ordering : kOrderings) {
      const GeneratorConfig config = config_with(5000, distribution, 77);
      EXPECT_EQ(generate_events(config, ordering), generate_events(config, ordering))
          << to_string(ordering) << " " << to_string(distribution);
    }
  }
}

TEST(Generator, DifferentSeedsGiveDifferentRows) {
  for (const DeviceDistribution distribution : kDistributions) {
    EXPECT_NE(generate_events(config_with(1000, distribution, 1), Ordering::TimeSorted),
              generate_events(config_with(1000, distribution, 2), Ordering::TimeSorted));
  }
}

TEST(Generator, AllOrderingsHoldTheSameMultiset) {
  for (const DeviceDistribution distribution : kDistributions) {
    const GeneratorConfig config = config_with(5000, distribution, 9);
    const auto time_sorted = generate_events(config, Ordering::TimeSorted);
    const auto expected = test::sorted_by_full_key(time_sorted);
    EXPECT_EQ(test::sorted_by_full_key(generate_events(config, Ordering::Shuffled)), expected);
    EXPECT_EQ(test::sorted_by_full_key(generate_events(config, Ordering::DeviceSorted)), expected);
  }
}

TEST(Generator, TimeSortedStartsAtTimestampStartAndStepsWithinBounds) {
  const GeneratorConfig config = config_with(10000);
  const auto rows = generate_events(config, Ordering::TimeSorted);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows.front().timestamp, config.timestamp_start);
  bool saw_equal_timestamps = false;
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const std::int64_t step = rows[i].timestamp - rows[i - 1].timestamp;
    ASSERT_GE(step, 0) << "row " << i;
    ASSERT_LE(step, config.max_timestamp_step) << "row " << i;
    saw_equal_timestamps = saw_equal_timestamps || step == 0;
  }
  EXPECT_TRUE(saw_equal_timestamps) << "default steps include 0, so duplicates are expected";
}

TEST(Generator, DeviceSortedIsOrderedByDeviceThenTimestamp) {
  for (const DeviceDistribution distribution : kDistributions) {
    const auto rows = generate_events(config_with(10000, distribution), Ordering::DeviceSorted);
    EXPECT_TRUE(std::is_sorted(rows.begin(), rows.end(), [](const Event& a, const Event& b) {
      return std::tie(a.device_id, a.timestamp) < std::tie(b.device_id, b.timestamp);
    })) << to_string(distribution);
  }
}

TEST(Generator, ShuffledIsNotTheTimeOrder) {
  const GeneratorConfig config = config_with(10000);
  const auto shuffled = generate_events(config, Ordering::Shuffled);
  EXPECT_NE(shuffled, generate_events(config, Ordering::TimeSorted));
  EXPECT_FALSE(std::is_sorted(shuffled.begin(), shuffled.end(), [](const Event& a, const Event& b) {
    return a.timestamp < b.timestamp;
  }));
}

// --- Value ranges and distributions -------------------------------------------------------------

TEST(Generator, ValuesStayInConfiguredRanges) {
  for (const DeviceDistribution distribution : kDistributions) {
    GeneratorConfig config = config_with(10000, distribution);
    config.device_count = 7;
    config.amount_min = -5;
    config.amount_max = 3;
    const auto rows = generate_events(config, Ordering::TimeSorted);
    bool saw_min = false;
    bool saw_max = false;
    for (const Event& row : rows) {
      ASSERT_GE(row.device_id, 0);
      ASSERT_LT(row.device_id, config.device_count);
      ASSERT_GE(row.amount, config.amount_min);
      ASSERT_LE(row.amount, config.amount_max);
      saw_min = saw_min || row.amount == config.amount_min;
      saw_max = saw_max || row.amount == config.amount_max;
    }
    EXPECT_TRUE(saw_min && saw_max) << "both amount bounds are reachable";
  }
}

TEST(Generator, DefaultConfigProducesNegativeAmounts) {
  const auto rows = generate_events(config_with(1000), Ordering::TimeSorted);
  EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](const Event& row) { return row.amount < 0; }));
}

TEST(Generator, UniformDeviceCountsAreWithinSixSigmaOfExpected) {
  GeneratorConfig config = config_with(100000, DeviceDistribution::Uniform, 5);
  config.device_count = 10;
  const auto counts = device_counts(generate_events(config, Ordering::TimeSorted), config.device_count);
  for (std::int32_t id = 0; id < config.device_count; ++id) {
    expect_count_near(counts[static_cast<std::size_t>(id)], 100000.0, 0.1, id);
  }
}

TEST(Generator, ZipfDeviceCountsAreWithinSixSigmaOfExpected) {
  GeneratorConfig config = config_with(100000, DeviceDistribution::Zipf, 5);
  config.device_count = 100;
  config.zipf_exponent = 1.0;
  const auto counts = device_counts(generate_events(config, Ordering::TimeSorted), config.device_count);

  double harmonic = 0.0;  // H_100 for s = 1
  for (int k = 1; k <= config.device_count; ++k) {
    harmonic += 1.0 / k;
  }
  for (std::int32_t id = 0; id < config.device_count; ++id) {
    expect_count_near(counts[static_cast<std::size_t>(id)], 100000.0, 1.0 / ((id + 1) * harmonic), id);
  }
  EXPECT_EQ(std::max_element(counts.begin(), counts.end()) - counts.begin(), 0)
      << "device 0 is the most frequent";
}

// --- Amount bound (overflow contract) -----------------------------------------------------------

TEST(GeneratorAmountBound, AcceptsTheExactBound) {
  GeneratorConfig config = config_with(4);
  config.amount_min = kInt64Max / 4;
  config.amount_max = kInt64Max / 4;
  const auto rows = generate_events(config, Ordering::TimeSorted);
  ASSERT_EQ(rows.size(), 4U);
  for (const Event& row : rows) {
    EXPECT_EQ(row.amount, 2'305'843'009'213'693'951);
  }
  const Query all = test::range(kInt64Min, kInt64Max);
  EXPECT_EQ(reference_evaluate(rows, all), (QueryResult{4, 9'223'372'036'854'775'804}));
  EXPECT_EQ(full_scan(RowStore{rows}, all), (QueryResult{4, 9'223'372'036'854'775'804}));
  EXPECT_EQ(full_scan(ColumnStore{rows}, all), (QueryResult{4, 9'223'372'036'854'775'804}));
}

TEST(GeneratorAmountBound, RejectsOneAboveTheBound) {
  GeneratorConfig above = config_with(4);
  above.amount_min = 0;
  above.amount_max = kInt64Max / 4 + 1;
  EXPECT_THROW((void)generate_events(above, Ordering::TimeSorted), std::invalid_argument);

  GeneratorConfig below = config_with(4);
  below.amount_min = -(kInt64Max / 4) - 1;
  below.amount_max = 0;
  EXPECT_THROW((void)generate_events(below, Ordering::TimeSorted), std::invalid_argument);
}

TEST(GeneratorAmountBound, IsConservative) {
  // Four rows of INT64_MIN / 4 would satisfy the amount-total invariant (they sum to INT64_MIN),
  // but |INT64_MIN / 4| > INT64_MAX / 4, so the generator's sufficient check rejects the config.
  GeneratorConfig config = config_with(4);
  config.amount_min = kInt64Min / 4;
  config.amount_max = kInt64Min / 4;
  EXPECT_THROW((void)generate_events(config, Ordering::TimeSorted), std::invalid_argument);
}

TEST(GeneratorAmountBound, ZeroRowsAcceptAnyAmountRange) {
  GeneratorConfig config = config_with(0);
  config.amount_min = kInt64Min;
  config.amount_max = kInt64Max;
  EXPECT_TRUE(generate_events(config, Ordering::TimeSorted).empty());
}

TEST(GeneratorAmountBound, OneRowAcceptsTheSymmetricFullRangeButNotInt64Min) {
  GeneratorConfig symmetric = config_with(1);
  symmetric.amount_min = -kInt64Max;
  symmetric.amount_max = kInt64Max;
  EXPECT_EQ(generate_events(symmetric, Ordering::TimeSorted).size(), 1U);

  GeneratorConfig with_min = config_with(1);
  with_min.amount_min = kInt64Min;
  with_min.amount_max = 0;
  EXPECT_THROW((void)generate_events(with_min, Ordering::TimeSorted), std::invalid_argument);
}

TEST(GeneratorAmountBound, TablesAtTheBoundAreAcceptedByStoresAndScanWithoutOverflow) {
  for (const Ordering ordering : kOrderings) {
    GeneratorConfig config = config_with(1000, DeviceDistribution::Uniform, 3);
    config.amount_min = -(kInt64Max / 1000);
    config.amount_max = kInt64Max / 1000;
    const auto rows = generate_events(config, ordering);
    const RowStore row_store{rows};
    const ColumnStore column_store{rows};
    const Query all = test::range(kInt64Min, kInt64Max);
    const QueryResult expected = reference_evaluate(rows, all);
    EXPECT_EQ(expected.count, 1000);
    EXPECT_EQ(full_scan(row_store, all), expected) << to_string(ordering);
    EXPECT_EQ(full_scan(column_store, all), expected) << to_string(ordering);
  }
}

// --- Timestamp bound and validation -------------------------------------------------------------

TEST(GeneratorTimestamps, AcceptsALastTimestampThatCanReachInt64Max) {
  GeneratorConfig config = config_with(11);
  config.timestamp_start = kInt64Max - 10;
  config.max_timestamp_step = 1;
  EXPECT_EQ(generate_events(config, Ordering::TimeSorted).size(), 11U);

  GeneratorConfig constant = config_with(1000);
  constant.timestamp_start = kInt64Max;
  constant.max_timestamp_step = 0;
  for (const Event& row : generate_events(constant, Ordering::TimeSorted)) {
    EXPECT_EQ(row.timestamp, kInt64Max);
  }
}

TEST(GeneratorTimestamps, RejectsAPossibleOverflow) {
  GeneratorConfig config = config_with(12);
  config.timestamp_start = kInt64Max - 10;
  config.max_timestamp_step = 1;
  EXPECT_THROW((void)generate_events(config, Ordering::TimeSorted), std::invalid_argument);
}

TEST(GeneratorTimestamps, AllowsANegativeStart) {
  GeneratorConfig config = config_with(1000);
  config.timestamp_start = kInt64Min;
  const auto rows = generate_events(config, Ordering::TimeSorted);
  ASSERT_EQ(rows.size(), 1000U);
  EXPECT_EQ(rows.front().timestamp, kInt64Min);
}

TEST(GeneratorValidation, RejectsInvalidConfigs) {
  const auto rejects = [](const GeneratorConfig& config) {
    EXPECT_THROW((void)generate_events(config, Ordering::TimeSorted), std::invalid_argument);
  };
  GeneratorConfig config = config_with(10);
  config.device_count = 0;
  rejects(config);
  config.device_count = -1;
  rejects(config);

  config = config_with(10);
  config.amount_min = 1;
  config.amount_max = 0;
  rejects(config);

  config = config_with(10);
  config.max_timestamp_step = -1;
  rejects(config);

  for (const double exponent : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()}) {
    config = config_with(10, DeviceDistribution::Zipf);
    config.zipf_exponent = exponent;
    rejects(config);
  }
}

TEST(GeneratorValidation, UniformIgnoresTheZipfExponent) {
  GeneratorConfig config = config_with(10, DeviceDistribution::Uniform);
  config.zipf_exponent = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(generate_events(config, Ordering::TimeSorted).size(), 10U);
}

// --- Determinism (scope: the documented toolchain) ----------------------------------------------

// Golden fingerprints pin the generator's output for the documented toolchain (Clang 18.1.3,
// libstdc++ 13, glibc 2.39, x86-64 Linux). They were recorded from the first debug-preset run and
// must then hold in every preset. A mismatch means the generated data changed. On another
// toolchain the Zipf rows may legitimately differ (see dataset.hpp), so a mismatch there is not by
// itself a bug.
struct Golden {
  DeviceDistribution distribution;
  Ordering ordering;
  std::uint64_t fingerprint;
};

TEST(GeneratorDeterminism, MatchesGoldenFingerprints) {
  constexpr std::array<Golden, 6> kGolden = {{
      {DeviceDistribution::Uniform, Ordering::TimeSorted, 7149942898011448490ULL},
      {DeviceDistribution::Uniform, Ordering::Shuffled, 5496920928238917034ULL},
      {DeviceDistribution::Uniform, Ordering::DeviceSorted, 11764744243818255710ULL},
      {DeviceDistribution::Zipf, Ordering::TimeSorted, 16181662328006314157ULL},
      {DeviceDistribution::Zipf, Ordering::Shuffled, 703142703228248997ULL},
      {DeviceDistribution::Zipf, Ordering::DeviceSorted, 14501335508094785597ULL},
  }};
  for (const Golden& golden : kGolden) {
    GeneratorConfig config = config_with(10000, golden.distribution, 20261125);
    config.device_count = 100;
    EXPECT_EQ(test::fingerprint(generate_events(config, golden.ordering)), golden.fingerprint)
        << to_string(golden.distribution) << " " << to_string(golden.ordering);
  }
}

}  // namespace
}  // namespace cqe
