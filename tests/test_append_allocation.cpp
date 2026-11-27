// This executable alone wraps ordinary scalar allocation. Fail each successive allocation
// inside append, with the injector disabled for setup, assertions and recovery.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <new>
#include <vector>

#include "cqe/executor.hpp"

namespace {
thread_local int fail_after = -1;
}

// The linker redirects calls to operator new(size_t) from project objects to this wrapper.
extern "C" void* __real__Znwm(std::size_t size);
extern "C" void* __wrap__Znwm(std::size_t size) {
  if (fail_after == 0) throw std::bad_alloc{};
  if (fail_after > 0) --fail_after;
  return __real__Znwm(size);
}

namespace cqe {
namespace {
void check_allocation_failures(bool compact) {
  const std::array first{Event{1, 5, 7}};
  std::vector<Event> batch(compact ? 70U : 4U, Event{3, 5, -1});
  batch.back().device_id = -7;
  for (int ordinal = 0; ; ++ordinal) {
    IndexedTable table{ZoneMappedTable{first, 1}};
    table.append(first);
    const auto& store = table.table().store();
    const auto zones = table.table().zone_map().zones();
    const auto times = store.timestamps();
    const auto ids = store.device_ids();
    const auto amounts = store.amounts();
    const auto posts = table.device_index().candidate_blocks(5);
    const auto totals = store.amount_totals();
    fail_after = ordinal;
    bool failed = false;
    try {
      table.append(batch);
    } catch (const std::bad_alloc&) {
      failed = true;
    }
    fail_after = -1;
    if (failed) {
      ASSERT_EQ(store.row_count(), 2U);
      EXPECT_EQ(store.amount_totals(), totals);
      ASSERT_EQ(times.data(), store.timestamps().data());
      ASSERT_EQ(ids.data(), store.device_ids().data());
      ASSERT_EQ(amounts.data(), store.amounts().data());
      ASSERT_EQ(zones.data(), table.table().zone_map().zones().data());
      EXPECT_TRUE(std::ranges::equal(times, std::array{1, 1}));
      EXPECT_TRUE(std::ranges::equal(ids, std::array{5, 5}));
      EXPECT_TRUE(std::ranges::equal(amounts, std::array{7, 7}));
      EXPECT_TRUE(std::ranges::all_of(zones, [](BlockZone z) { return z == BlockZone{1, 1, 5, 5}; }));
      ASSERT_TRUE(std::ranges::equal(posts, std::array{0U, 1U}));
      EXPECT_EQ(table.device_index().merge_count(), 0U);
      EXPECT_TRUE(table.device_index().candidate_blocks(-7).empty());
      table.append(batch);
    }
    ASSERT_EQ(store.row_count(), 2 + batch.size());
    ASSERT_EQ(table.device_index().merge_count(), compact ? 1U : 0U);
    ASSERT_EQ(index_scan(table, DeviceEquals{5}).result.count, static_cast<std::int64_t>(1 + batch.size()));
    if (!failed) {
      EXPECT_GT(ordinal, 0);
      break;
    }
  }
}
TEST(AppendAllocation, OrdinaryGrowth) { check_allocation_failures(false); }
TEST(AppendAllocation, DeltaCompaction) { check_allocation_failures(true); }
}  // namespace
}  // namespace cqe
