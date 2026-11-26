#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "cqe/block_index.hpp"
#include "cqe/dataset.hpp"

namespace cqe {
namespace {

std::vector<std::size_t> postings(const IndexedTable& table, std::int32_t id) {
  const auto blocks = table.device_index().candidate_blocks(id);
  return {blocks.begin(), blocks.end()};
}

TEST(DeviceBlockIndex, EmptyTableHasNoPostingsOrLogicalBytes) {
  const IndexedTable table{ZoneMappedTable{{}, 3}};
  EXPECT_TRUE(table.device_index().candidate_blocks(0).empty());
  EXPECT_EQ(table.device_index().logical_bytes(), 0U);
}

TEST(DeviceBlockIndex, CommonAndRareDevicesHaveSortedUniquePostings) {
  const std::array<Event, 10> rows{{
      {0, 7, 5}, {1, 7, -9}, {2, 3, 20}, {3, 7, 10}, {4, 8, 4},
      {5, 8, 5}, {6, 4, 1}, {7, 7, -3}, {8, 9, 8}, {9, -1, -11},
  }};
  const IndexedTable table{ZoneMappedTable{rows, 3}};
  EXPECT_EQ(postings(table, 7), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(postings(table, 8), (std::vector<std::size_t>{1}));
  EXPECT_EQ(postings(table, -1), (std::vector<std::size_t>{3}));
  EXPECT_TRUE(postings(table, 6).empty());
  EXPECT_GT(table.device_index().logical_bytes(), 0U);
}

TEST(DeviceBlockIndex, RepeatingARowWithinABlockDoesNotIncreaseLogicalBytes) {
  const std::array<Event, 2> rows{{{0, 7, 5}, {1, 7, -9}}};
  const IndexedTable repeated{ZoneMappedTable{rows, 2}};
  const IndexedTable single{ZoneMappedTable{std::span{rows}.first(1), 2}};
  EXPECT_EQ(postings(repeated, 7), (std::vector<std::size_t>{0}));
  EXPECT_EQ(repeated.device_index().logical_bytes(), single.device_index().logical_bytes());
}

TEST(DeviceBlockIndex, TableKeepsItsOwnPostingsThroughCopyAndMove) {
  constexpr auto min = std::numeric_limits<std::int32_t>::min();
  constexpr auto max = std::numeric_limits<std::int32_t>::max();
  const std::array<Event, 2> rows{{{0, min, 3}, {1, max, 4}}};
  const std::array<Event, 2> reversed{{rows[1], rows[0]}};
  const IndexedTable first{ZoneMappedTable{rows, 1}};
  const IndexedTable second{ZoneMappedTable{reversed, 1}};
  IndexedTable copied = first;
  const IndexedTable moved{std::move(copied)};
  EXPECT_EQ(postings(first, min), (std::vector<std::size_t>{0}));
  EXPECT_EQ(postings(second, min), (std::vector<std::size_t>{1}));
  EXPECT_EQ(postings(moved, min), (std::vector<std::size_t>{0}));
  EXPECT_EQ(postings(moved, max), (std::vector<std::size_t>{1}));
  EXPECT_EQ(moved.device_index().logical_bytes(), first.device_index().logical_bytes());
  EXPECT_EQ(copied.table().store().row_count(), 0U);
  EXPECT_TRUE(postings(copied, min).empty());
  EXPECT_EQ(copied.device_index().logical_bytes(), 0U);
}

TEST(DeviceBlockIndex, EveryPostingMatchesAnIndependentRowEnumeration) {
  GeneratorConfig config;
  config.row_count = 2'051;
  config.device_count = 17;
  for (const Ordering ordering : {Ordering::TimeSorted, Ordering::Shuffled, Ordering::DeviceSorted}) {
    const auto rows = generate_events(config, ordering);
    for (const std::size_t capacity : std::array<std::size_t, 3>{1, 16, 1024}) {
      const IndexedTable table{ZoneMappedTable{rows, capacity}};
      for (std::int32_t id = -1; id <= config.device_count; ++id) {
        std::vector<std::size_t> expected;
        for (std::size_t row = 0; row < rows.size(); ++row) {
          const std::size_t block = row / capacity;
          if (rows[row].device_id == id && (expected.empty() || expected.back() != block)) {
            expected.push_back(block);
          }
        }
        ASSERT_EQ(postings(table, id), expected)
            << "ordering=" << to_string(ordering) << ", capacity=" << capacity << ", device=" << id;
      }
    }
  }
}

}  // namespace
}  // namespace cqe
