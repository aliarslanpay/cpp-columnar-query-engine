#include "cqe/column_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "cqe/executor.hpp"
#include "test_util.hpp"

namespace cqe {
namespace {

using test::kInt32Max;
using test::kInt32Min;
using test::kInt64Max;
using test::kInt64Min;

TEST(ColumnStore, EmptyTable) {
  const std::vector<Event> rows;
  const ColumnStore store{rows};
  EXPECT_EQ(store.row_count(), 0U);
  EXPECT_TRUE(store.timestamps().empty());
  EXPECT_TRUE(store.device_ids().empty());
  EXPECT_TRUE(store.amounts().empty());
  EXPECT_EQ(store.amount_totals(), (AmountTotals{0, 0}));
}

TEST(ColumnStore, TransposesRowsInOrder) {
  const std::vector<Event> rows = {
      {30, 1, -4}, {10, kInt32Max, 5}, {kInt64Min, kInt32Min, 0}, {kInt64Max, 0, 7}};
  const ColumnStore store{rows};
  ASSERT_EQ(store.row_count(), rows.size());
  ASSERT_EQ(store.timestamps().size(), rows.size());
  ASSERT_EQ(store.device_ids().size(), rows.size());
  ASSERT_EQ(store.amounts().size(), rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(store.timestamps()[i], rows[i].timestamp) << "row " << i;
    EXPECT_EQ(store.device_ids()[i], rows[i].device_id) << "row " << i;
    EXPECT_EQ(store.amounts()[i], rows[i].amount) << "row " << i;
  }
  EXPECT_EQ(store.amount_totals(), (AmountTotals{12, -4}));
}

TEST(ColumnStore, CopiesItsInput) {
  std::vector<Event> rows = {{1, 1, 1}};
  const ColumnStore store{rows};
  rows[0] = Event{2, 2, 2};
  EXPECT_EQ(store.timestamps()[0], 1);
  EXPECT_EQ(store.device_ids()[0], 1);
  EXPECT_EQ(store.amounts()[0], 1);
}

// Regression: the implicit move constructor emptied the source's columns but left its amount
// totals unchanged, so the moved-from store broke the invariant.
TEST(ColumnStore, MoveLeavesTheSourceAsAConsistentEmptyTable) {
  const std::vector<Event> rows = {{1, 1, 5}, {2, 2, -3}};
  ColumnStore source{rows};
  const ColumnStore target{std::move(source)};
  EXPECT_EQ(target.row_count(), 2U);
  EXPECT_EQ(target.amount_totals(), (AmountTotals{5, -3}));
  EXPECT_EQ(target.timestamps()[1], 2);
  EXPECT_EQ(target.device_ids()[1], 2);
  EXPECT_EQ(target.amounts()[1], -3);
  // Inspecting the moved-from state is the point of this test.
  EXPECT_EQ(source.row_count(), 0U);
  EXPECT_TRUE(source.timestamps().empty());
  EXPECT_TRUE(source.device_ids().empty());
  EXPECT_TRUE(source.amounts().empty());
  EXPECT_EQ(source.amount_totals(), (AmountTotals{0, 0}));
  EXPECT_EQ(full_scan(source, test::range(kInt64Min, kInt64Max)), (QueryResult{0, 0}));
}

// Compile-time regression: only the special members that preserve the invariants exist.
TEST(ColumnStore, OnlyInvariantPreservingSpecialMembersAreProvided) {
  static_assert(std::is_copy_constructible_v<ColumnStore>);
  static_assert(std::is_nothrow_move_constructible_v<ColumnStore>);
  static_assert(!std::is_copy_assignable_v<ColumnStore>);
  static_assert(!std::is_move_assignable_v<ColumnStore>);
  SUCCEED();
}

TEST(ColumnStore, CopyIsIndependentAndConsistent) {
  const std::vector<Event> rows = {{1, 1, 5}, {2, 2, -3}};
  const ColumnStore original{rows};
  const ColumnStore copy{original};
  EXPECT_EQ(copy.row_count(), original.row_count());
  EXPECT_EQ(copy.amount_totals(), original.amount_totals());
  EXPECT_TRUE(std::equal(original.amounts().begin(), original.amounts().end(),
                         copy.amounts().begin(), copy.amounts().end()));
  EXPECT_NE(copy.amounts().data(), original.amounts().data());
}

TEST(ColumnStore, RejectsRowsThatViolateTheAmountTotalInvariant) {
  const std::vector<Event> too_positive = {{0, 0, kInt64Max}, {1, 0, 1}};
  const std::vector<Event> too_negative = {{0, 0, kInt64Min}, {1, 0, -1}};
  EXPECT_THROW(ColumnStore{too_positive}, std::invalid_argument);
  EXPECT_THROW(ColumnStore{too_negative}, std::invalid_argument);
}

}  // namespace
}  // namespace cqe
