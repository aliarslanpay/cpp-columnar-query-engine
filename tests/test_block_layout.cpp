#include "cqe/block_layout.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <stdexcept>

#include "zone_test_util.hpp"

namespace cqe {
namespace {

constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

TEST(BlockLayout, RejectsZeroCapacity) {
  EXPECT_THROW(BlockLayout(10, 0), std::invalid_argument);
  EXPECT_THROW(BlockLayout(0, 0), std::invalid_argument);
}

TEST(BlockLayout, EmptyTableHasNoBlocks) {
  for (const std::size_t capacity : {std::size_t{1}, std::size_t{1024}, kSizeMax}) {
    const BlockLayout layout(0, capacity);
    EXPECT_EQ(layout.block_count(), 0U) << "capacity " << capacity;
    EXPECT_THROW((void)layout.block(0), std::out_of_range) << "capacity " << capacity;
  }
}

TEST(BlockLayout, ExactMultipleHasOnlyFullBlocks) {
  const BlockLayout layout(2048, 1024);
  ASSERT_EQ(layout.block_count(), 2U);
  EXPECT_EQ(layout.block(0), (BlockRange{0, 1024}));
  EXPECT_EQ(layout.block(1), (BlockRange{1024, 2048}));
}

TEST(BlockLayout, PartialLastBlock) {
  const BlockLayout one_extra(1025, 1024);
  ASSERT_EQ(one_extra.block_count(), 2U);
  EXPECT_EQ(one_extra.block(1), (BlockRange{1024, 1025}));

  const BlockLayout one_short(1023, 1024);
  ASSERT_EQ(one_short.block_count(), 1U);
  EXPECT_EQ(one_short.block(0), (BlockRange{0, 1023}));

  const BlockLayout small(10, 4);
  ASSERT_EQ(small.block_count(), 3U);
  EXPECT_EQ(small.block(0), (BlockRange{0, 4}));
  EXPECT_EQ(small.block(1), (BlockRange{4, 8}));
  EXPECT_EQ(small.block(2), (BlockRange{8, 10}));
}

TEST(BlockLayout, CapacityOneGivesOneRowPerBlock) {
  const BlockLayout layout(5, 1);
  ASSERT_EQ(layout.block_count(), 5U);
  for (std::size_t b = 0; b < 5; ++b) {
    EXPECT_EQ(layout.block(b), (BlockRange{b, b + 1}));
  }
}

TEST(BlockLayout, CapacityLargerThanTableGivesOnePartialBlock) {
  const BlockLayout layout(7, 1024);
  ASSERT_EQ(layout.block_count(), 1U);
  EXPECT_EQ(layout.block(0), (BlockRange{0, 7}));
}

TEST(BlockLayout, OutOfRangeBlockIndexThrows) {
  const BlockLayout layout(10, 4);
  EXPECT_THROW((void)layout.block(3), std::out_of_range);
  EXPECT_THROW((void)layout.block(kSizeMax), std::out_of_range);
}

// The blocks cover [0, n) contiguously; all are full except possibly the last, which holds
// between 1 and capacity rows. The expected count is found by walking, not by a formula.
TEST(BlockLayout, BlocksTileTheRowsExactly) {
  constexpr std::size_t kRowCounts[] = {1, 2, 3, 7, 1023, 1024, 1025, 5000};
  constexpr std::size_t kCapacities[] = {1, 2, 3, 7, 1024, 4096};
  for (const std::size_t n : kRowCounts) {
    for (const std::size_t capacity : kCapacities) {
      const BlockLayout layout(n, capacity);
      std::size_t expected_blocks = 0;
      for (std::size_t covered = 0; covered < n; covered += capacity) {
        ++expected_blocks;
      }
      ASSERT_EQ(layout.block_count(), expected_blocks) << "n " << n << " capacity " << capacity;
      std::size_t next_begin = 0;
      for (std::size_t b = 0; b < layout.block_count(); ++b) {
        const BlockRange range = layout.block(b);
        EXPECT_EQ(range.begin, next_begin) << "n " << n << " capacity " << capacity << " block " << b;
        EXPECT_GE(range.size(), 1U);
        EXPECT_LE(range.size(), capacity);
        if (b + 1 < layout.block_count()) {
          EXPECT_EQ(range.size(), capacity) << "only the last block may be partial";
        }
        next_begin = range.end;
      }
      EXPECT_EQ(next_begin, n) << "n " << n << " capacity " << capacity;
    }
  }
}

TEST(BlockLayout, CapacitySizeMaxDoesNotOverflow) {
  const BlockLayout small(5, kSizeMax);
  ASSERT_EQ(small.block_count(), 1U);
  EXPECT_EQ(small.block(0), (BlockRange{0, 5}));

  const BlockLayout huge(kSizeMax, kSizeMax);
  ASSERT_EQ(huge.block_count(), 1U);
  EXPECT_EQ(huge.block(0), (BlockRange{0, kSizeMax}));
}

// (b + 1) * capacity would wrap here; begin + min(capacity, row_count - begin) does not.
TEST(BlockLayout, SecondBlockOfSizeMaxRowsWithCapacitySizeMaxMinusOne) {
  const BlockLayout layout(kSizeMax, kSizeMax - 1);
  ASSERT_EQ(layout.block_count(), 2U);
  EXPECT_EQ(layout.block(0), (BlockRange{0, kSizeMax - 1}));
  EXPECT_EQ(layout.block(1), (BlockRange{kSizeMax - 1, kSizeMax}));
  EXPECT_THROW((void)layout.block(2), std::out_of_range);
}

TEST(BlockLayout, LastBlockOfSizeMaxRowsWithCapacityTwo) {
  const BlockLayout layout(kSizeMax, 2);
  const std::size_t expected_blocks = kSizeMax / 2 + 1;  // SIZE_MAX is odd
  ASSERT_EQ(layout.block_count(), expected_blocks);
  EXPECT_EQ(layout.block(expected_blocks - 1), (BlockRange{kSizeMax - 1, kSizeMax}));
  EXPECT_EQ(layout.block(expected_blocks - 2), (BlockRange{kSizeMax - 3, kSizeMax - 1}));
}

}  // namespace
}  // namespace cqe
