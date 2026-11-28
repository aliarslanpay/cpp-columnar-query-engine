// This executable alone wraps ordinary scalar allocation. Each scenario fails successive
// allocations inside one append, with the injector disabled for setup, checks and retries.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
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

// The table under test is built from `initial` followed by the `setup` appends. `batch` is the
// append whose allocations fail one at a time.
struct Scenario {
  std::size_t capacity = 1;
  std::vector<Event> initial;
  std::vector<std::vector<Event>> setup;
  std::vector<Event> batch;
  std::size_t merges = 0;  // compactions performed by a successful `batch` append
  // Every buffer already has room for `batch`: success must not replace any of them.
  bool in_place = false;
};

IndexedTable build_table(const Scenario& s) {
  IndexedTable table{ZoneMappedTable{s.initial, s.capacity}};
  for (const auto& rows : s.setup) table.append(rows);
  return table;
}

std::vector<Event> rows_before(const Scenario& s) {
  std::vector<Event> rows = s.initial;
  for (const auto& batch : s.setup) rows.insert(rows.end(), batch.begin(), batch.end());
  return rows;
}

std::vector<std::int32_t> devices_of(std::span<const Event> rows) {
  std::vector<std::int32_t> ids;
  for (const auto& row : rows) ids.push_back(row.device_id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

// Views borrowed before the append, and the address of every posting they show.
struct Borrowed {
  std::span<const std::int64_t> timestamps;
  std::span<const std::int32_t> device_ids;
  std::span<const std::int64_t> amounts;
  std::span<const BlockZone> zones;
  std::vector<PostingView> postings;  // one per device
  std::vector<std::vector<const std::size_t*>> addresses;
};

Borrowed borrow(const IndexedTable& table, std::span<const std::int32_t> devices) {
  const auto& store = table.table().store();
  Borrowed views{store.timestamps(), store.device_ids(), store.amounts(),
                 table.table().zone_map().zones(), {}, {}};
  for (const auto id : devices) {
    const auto view = table.device_index().candidate_blocks(id);
    views.postings.push_back(view);
    auto& addresses = views.addresses.emplace_back();
    for (const auto& block : view) addresses.push_back(&block);
  }
  return views;
}

// The first addresses.size() postings of `live` are the recorded objects.
bool starts_with(const PostingView& live, const std::vector<const std::size_t*>& addresses) {
  if (live.size() < addresses.size()) return false;
  auto it = live.begin();
  for (const auto* address : addresses) {
    if (&*it != address) return false;
    ++it;
  }
  return true;
}

// Columns, amount totals, layout, zones, postings and index scans equal those of `expected`.
void expect_same_table(const IndexedTable& actual, const IndexedTable& expected,
                       std::span<const std::int32_t> devices) {
  const auto& store = actual.table().store();
  const auto& want = expected.table().store();
  EXPECT_TRUE(std::ranges::equal(store.timestamps(), want.timestamps()));
  EXPECT_TRUE(std::ranges::equal(store.device_ids(), want.device_ids()));
  EXPECT_TRUE(std::ranges::equal(store.amounts(), want.amounts()));
  EXPECT_EQ(store.amount_totals(), want.amount_totals());
  const auto& layout = actual.table().zone_map().layout();
  const auto& want_layout = expected.table().zone_map().layout();
  EXPECT_EQ(layout.row_count(), want_layout.row_count());
  EXPECT_EQ(layout.capacity(), want_layout.capacity());
  EXPECT_EQ(layout.block_count(), want_layout.block_count());
  EXPECT_TRUE(std::ranges::equal(actual.table().zone_map().zones(),
                                 expected.table().zone_map().zones()));
  for (const auto id : devices) {
    SCOPED_TRACE(::testing::Message() << "device " << id);
    EXPECT_TRUE(std::ranges::equal(actual.device_index().candidate_blocks(id),
                                   expected.device_index().candidate_blocks(id)));
    EXPECT_EQ(index_scan(actual, DeviceEquals{id}), index_scan(expected, DeviceEquals{id}));
  }
}

template <class T>
void expect_view_intact(std::span<const T> old, std::span<const T> live, std::span<const T> want) {
  EXPECT_EQ(old.data(), live.data());
  EXPECT_EQ(old.size(), live.size());
  EXPECT_TRUE(std::ranges::equal(old, want));
}

// After a failed append the borrowed views still name the live buffers, and reading through
// them gives the old rows, zones and postings. ASan reports a read of a released buffer.
void expect_views_intact(const Borrowed& views, const IndexedTable& table,
                         const IndexedTable& expected, std::span<const std::int32_t> devices) {
  const auto& store = table.table().store();
  const auto& want = expected.table().store();
  expect_view_intact(views.timestamps, store.timestamps(), want.timestamps());
  expect_view_intact(views.device_ids, store.device_ids(), want.device_ids());
  expect_view_intact(views.amounts, store.amounts(), want.amounts());
  expect_view_intact(views.zones, table.table().zone_map().zones(),
                     expected.table().zone_map().zones());
  for (std::size_t i = 0; i < devices.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "device " << devices[i]);
    const auto live = table.device_index().candidate_blocks(devices[i]);
    EXPECT_EQ(views.postings[i].size(), live.size());
    EXPECT_TRUE(starts_with(live, views.addresses[i]));
    EXPECT_TRUE(std::ranges::equal(views.postings[i],
                                   expected.device_index().candidate_blocks(devices[i])));
  }
}

// After a successful in-place append every recorded buffer and posting is still live. Only
// addresses are compared; the expired views are not read.
void expect_buffers_kept(const Borrowed& views, const IndexedTable& table,
                         std::span<const std::int32_t> devices) {
  const auto& store = table.table().store();
  EXPECT_EQ(views.timestamps.data(), store.timestamps().data());
  EXPECT_EQ(views.device_ids.data(), store.device_ids().data());
  EXPECT_EQ(views.amounts.data(), store.amounts().data());
  EXPECT_EQ(views.zones.data(), table.table().zone_map().zones().data());
  for (std::size_t i = 0; i < devices.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "device " << devices[i]);
    EXPECT_TRUE(starts_with(table.device_index().candidate_blocks(devices[i]), views.addresses[i]));
  }
}

// Fails the first, second, ... allocation of the `batch` append until an attempt succeeds.
// After each failure the table and its borrowed views must be unchanged. The retry on the same
// table, and the attempt without a failure, must both match a table built from scratch.
void check_allocation_failures(const Scenario& s, const std::string& property) {
  const auto before = rows_before(s);
  auto after = before;
  after.insert(after.end(), s.batch.begin(), s.batch.end());
  const auto devices = devices_of(after);
  const IndexedTable old_rows{ZoneMappedTable{before, s.capacity}};
  const IndexedTable new_rows{ZoneMappedTable{after, s.capacity}};
  for (int ordinal = 0;; ++ordinal) {
    SCOPED_TRACE(::testing::Message() << "allocation " << ordinal << " fails");
    IndexedTable table = build_table(s);
    const auto views = borrow(table, devices);
    const auto merges = table.device_index().merge_count();
    const auto bytes = table.device_index().logical_bytes();
    fail_after = ordinal;
    bool failed = false;
    try {
      table.append(s.batch);
    } catch (const std::bad_alloc&) {
      failed = true;
    }
    fail_after = -1;
    if (failed) {
      expect_views_intact(views, table, old_rows, devices);
      expect_same_table(table, old_rows, devices);
      EXPECT_EQ(table.device_index().merge_count(), merges);
      EXPECT_EQ(table.device_index().logical_bytes(), bytes);
      table.append(s.batch);  // retry on the same table
    }
    expect_same_table(table, new_rows, devices);
    EXPECT_EQ(table.device_index().merge_count(), merges + s.merges);
    if (s.in_place) expect_buffers_kept(views, table, devices);
    if (!failed) {
      EXPECT_GT(ordinal, 0);  // the wrapper intercepted at least one allocation
      ::testing::Test::RecordProperty(property, ordinal);
      return;
    }
  }
}

// Block capacity 1 and full buffers: the columns, the zones and device 5's delta all grow.
Scenario growth(std::size_t rows) {
  Scenario s;
  s.initial = {Event{1, 5, 7}};
  s.setup = {{Event{1, 5, 7}}};
  s.batch.assign(rows, Event{3, 5, -1});
  s.batch.back().device_id = -7;
  return s;
}

TEST(AppendAllocation, OrdinaryGrowth) {
  check_allocation_failures(growth(4), "injected_failures");
}

TEST(AppendAllocation, DeltaCompaction) {
  auto s = growth(70);
  s.merges = 1;
  check_allocation_failures(s, "injected_failures");
}

// Rows 0..2 at capacity 4 leave block 0 partial. The columns have room for one more row and no
// new zone is needed, so the fourth row fills block 0 in place and only the index allocates.
// Block 0 is already a base posting of device 5 and a delta posting of device 6; device -7
// gains a new posting.
TEST(AppendAllocation, PartialLastBlockFillsExistingBuffers) {
  for (const std::int32_t device : {5, 6, -7}) {
    SCOPED_TRACE(::testing::Message() << "fourth row device " << device);
    Scenario s;
    s.capacity = 4;
    s.initial = {Event{1, 5, 7}};
    s.setup = {{Event{3, 6, 7}}, {Event{2, 5, 7}}};
    s.batch = {Event{9, device, -2}};
    s.in_place = true;
    check_allocation_failures(s, "injected_failures_device_" + std::to_string(device));
  }
}

// Device 5 has base postings 0..3 and delta postings 4..6. Growing the delta from two to three
// postings doubled its capacity to four, and the columns and zones have room for an eighth row,
// so block 7 is appended to the spare delta slot without replacing any buffer.
TEST(AppendAllocation, DeltaPostingFillsSpareCapacity) {
  Scenario s;
  s.initial = {Event{1, 5, 7}, Event{2, 5, 7}, Event{3, 5, 7}, Event{4, 5, 7}};
  s.setup = {{Event{5, 5, 7}}, {Event{6, 5, 7}}, {Event{7, 5, 7}}};
  s.batch = {Event{8, 5, -2}};
  s.in_place = true;
  check_allocation_failures(s, "injected_failures");
}

}  // namespace
}  // namespace cqe
