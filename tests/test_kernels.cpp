#include "cqe/kernels.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "test_util.hpp"

namespace cqe {
namespace {

using test::kInt32Max;
using test::kInt32Min;
using test::kInt64Max;
using test::kInt64Min;

// The offset type covers a slice of kMaxSelectableRows rows: its last offset is UINT32_MAX.
static_assert(sizeof(RowOffset) == 4);
static_assert(kMaxSelectableRows == std::uint64_t{4294967296});
static_assert(kMaxSelectableRows - 1 == std::numeric_limits<RowOffset>::max());

constexpr RowOffset kSentinel = 0xDEADBEEF;
constexpr std::size_t kSlack = 8;

template <class T, class Match>
std::vector<RowOffset> brute_force(const std::vector<T>& column, Match match) {
  std::vector<RowOffset> selected;
  for (std::size_t i = 0; i < column.size(); ++i) {
    if (match(column[i])) {
      selected.push_back(static_cast<RowOffset>(i));
    }
  }
  return selected;
}

// Runs a kernel with an output buffer kSlack entries longer than the column, filled with a
// sentinel, and checks that nothing at index column.size() or above was written.
template <class Kernel>
std::vector<RowOffset> run_kernel(std::size_t column_size, Kernel kernel) {
  std::vector<RowOffset> out(column_size + kSlack, kSentinel);
  const std::size_t k = kernel(std::span<RowOffset>(out));
  EXPECT_LE(k, column_size);
  for (std::size_t i = column_size; i < out.size(); ++i) {
    EXPECT_EQ(out[i], kSentinel) << "wrote beyond the column size at " << i;
  }
  return {out.begin(), out.begin() + static_cast<std::ptrdiff_t>(k)};
}

std::vector<RowOffset> range_select(const std::vector<std::int64_t>& column, std::int64_t lo,
                                    std::int64_t hi) {
  return run_kernel(column.size(),
                    [&](std::span<RowOffset> out) { return select_range(column, lo, hi, out); });
}

std::vector<RowOffset> equals_select(const std::vector<std::int32_t>& column, std::int32_t value) {
  return run_kernel(column.size(),
                    [&](std::span<RowOffset> out) { return select_equals(column, value, out); });
}

using Offsets = std::vector<RowOffset>;

TEST(SelectRange, EmptyColumn) {
  const std::vector<std::int64_t> column;
  EXPECT_EQ(range_select(column, kInt64Min, kInt64Max), Offsets{});
  EXPECT_EQ(select_range(column, 0, 0, std::span<RowOffset>{}), 0U);
}

TEST(SelectRange, AllNoneInvertedAndPoint) {
  const std::vector<std::int64_t> column = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_EQ(range_select(column, kInt64Min, kInt64Max), (Offsets{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
  EXPECT_EQ(range_select(column, 11, 20), Offsets{});
  EXPECT_EQ(range_select(column, 5, 4), Offsets{});
  EXPECT_EQ(range_select(column, 5, 5), Offsets{4});
  EXPECT_EQ(range_select(column, 3, 6), (Offsets{2, 3, 4, 5}));
}

TEST(SelectRange, Int64Limits) {
  const std::vector<std::int64_t> column = {kInt64Min, 0, kInt64Max, kInt64Min, kInt64Max};
  EXPECT_EQ(range_select(column, kInt64Min, kInt64Min), (Offsets{0, 3}));
  EXPECT_EQ(range_select(column, kInt64Max, kInt64Max), (Offsets{2, 4}));
  EXPECT_EQ(range_select(column, kInt64Min, kInt64Max), (Offsets{0, 1, 2, 3, 4}));
  EXPECT_EQ(range_select(column, kInt64Min + 1, kInt64Max - 1), Offsets{1});
  EXPECT_EQ(range_select(column, kInt64Max, kInt64Min), Offsets{});
}

TEST(SelectRange, ExactOutputSizeIsEnough) {
  const std::vector<std::int64_t> column = {1, 2, 3};
  std::vector<RowOffset> out(column.size());
  EXPECT_EQ(select_range(column, kInt64Min, kInt64Max, out), 3U);
  EXPECT_EQ(out, (Offsets{0, 1, 2}));
}

TEST(SelectRange, MatchesBruteForce) {
  std::mt19937_64 rng(7);
  std::uniform_int_distribution<std::int64_t> value(-8, 8);
  std::uniform_int_distribution<std::int64_t> bound(-10, 10);
  constexpr std::size_t kSizes[] = {0, 1, 2, 3, 7, 8, 9, 31, 32, 33, 1023, 1024, 1025};
  for (const std::size_t n : kSizes) {
    std::vector<std::int64_t> column(n);
    for (std::int64_t& v : column) {
      v = value(rng);
    }
    for (int q = 0; q < 50; ++q) {
      const std::int64_t lo = bound(rng);
      const std::int64_t hi = bound(rng);
      EXPECT_EQ(range_select(column, lo, hi),
                brute_force(column, [&](std::int64_t v) { return lo <= v && v <= hi; }))
          << "n " << n << " [" << lo << ", " << hi << "]";
    }
  }
}

TEST(SelectEquals, EdgesAndInt32Limits) {
  const std::vector<std::int32_t> column = {kInt32Min, 0, kInt32Max, 0, kInt32Min};
  EXPECT_EQ(equals_select(column, kInt32Min), (Offsets{0, 4}));
  EXPECT_EQ(equals_select(column, kInt32Max), Offsets{2});
  EXPECT_EQ(equals_select(column, 0), (Offsets{1, 3}));
  EXPECT_EQ(equals_select(column, 1), Offsets{});
  EXPECT_EQ(equals_select(std::vector<std::int32_t>{}, 0), Offsets{});
  EXPECT_EQ(equals_select(std::vector<std::int32_t>(6, 3), 3), (Offsets{0, 1, 2, 3, 4, 5}));
}

TEST(SelectEquals, MatchesBruteForce) {
  std::mt19937_64 rng(8);
  std::uniform_int_distribution<std::int32_t> value(-4, 4);
  constexpr std::size_t kSizes[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 1023, 1024, 1025};
  for (const std::size_t n : kSizes) {
    std::vector<std::int32_t> column(n);
    for (std::int32_t& v : column) {
      v = value(rng);
    }
    for (std::int32_t target = -5; target <= 5; ++target) {
      EXPECT_EQ(equals_select(column, target),
                brute_force(column, [&](std::int32_t v) { return v == target; }))
          << "n " << n << " value " << target;
    }
  }
}

TEST(SumSelected, ReadsOnlyTheSelectedOffsets) {
  const std::vector<std::int64_t> column = {5, -3, 7, 100, -50};
  EXPECT_EQ(sum_selected(column, Offsets{}), 0);
  EXPECT_EQ(sum_selected(column, Offsets{0, 2}), 12);
  EXPECT_EQ(sum_selected(column, Offsets{1}), -3);
  EXPECT_EQ(sum_selected(column, Offsets{0, 1, 2, 3, 4}), 59);
  EXPECT_EQ(sum_selected(column, Offsets{4}), -50);
}

TEST(SumSelected, SumsAtTheInt64Limits) {
  const std::vector<std::int64_t> column = {kInt64Max, 1, kInt64Min};
  EXPECT_EQ(sum_selected(column, Offsets{0, 2}), -1);
  EXPECT_EQ(sum_selected(column, Offsets{0}), kInt64Max);
  EXPECT_EQ(sum_selected(column, Offsets{2}), kInt64Min);
}

TEST(SumSelected, AfterSelectRangeEqualsAFilteredSum) {
  std::mt19937_64 rng(9);
  std::uniform_int_distribution<std::int64_t> value(-1000, 1000);
  std::vector<std::int64_t> keys(2000);
  std::vector<std::int64_t> amounts(2000);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    keys[i] = value(rng);
    amounts[i] = value(rng);
  }
  std::vector<RowOffset> out(keys.size());
  const std::size_t k = select_range(keys, -100, 250, out);
  std::int64_t expected = 0;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (-100 <= keys[i] && keys[i] <= 250) {
      expected += amounts[i];
    }
  }
  EXPECT_EQ(sum_selected(amounts, std::span<const RowOffset>(out).first(k)), expected);
}

}  // namespace
}  // namespace cqe
