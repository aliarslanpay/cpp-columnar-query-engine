#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <ranges>
#include <vector>

#include "cqe/executor.hpp"
#include "cqe/reference.hpp"

namespace cqe {
namespace {

static_assert(std::ranges::forward_range<PostingView>);
static_assert(std::ranges::sized_range<PostingView>);

TEST(Append, RebuildAndRowOracleAgreeAfterEveryBatch) {
  EXPECT_TRUE(PostingView{}.empty());
  for (const auto seed : {11U, 37U, 97U}) {
    std::mt19937 rng(seed);
    for (const std::size_t cap : std::array<std::size_t, 3>{1, 7, 32}) {
      std::vector<Event> history;
      IndexedTable table{ZoneMappedTable{history, cap}};
      ZoneMappedTable standalone{history, cap};
      for (std::size_t step = 0; step < 36; ++step) {
        const std::array sizes{std::size_t{0}, std::size_t{1}, cap - 1, cap, cap + 1,
                               std::size_t{2} + rng() % (3 * cap)};
        const auto size = sizes[rng() % sizes.size()];
        SCOPED_TRACE(::testing::Message() << seed << "/" << cap << "/" << step << "/" << size);
        std::vector<Event> batch;
        for (std::size_t i = 0; i < size; ++i) {
          batch.push_back({static_cast<int>(rng() % 43) - 21, static_cast<int>(rng() % 11) - 5,
                           static_cast<int>(rng() % 101) - 50});
        }
        table.append(batch);
        standalone.append(batch);
        history.insert(history.end(), batch.begin(), batch.end());
        const IndexedTable rebuilt{ZoneMappedTable{history, cap}};
        const auto& baseline = rebuilt.table();
        for (const ZoneMappedTable* current :
             std::array<const ZoneMappedTable*, 2>{&table.table(), &standalone}) {
          ASSERT_TRUE(std::ranges::equal(current->zone_map().zones(), baseline.zone_map().zones()));
          EXPECT_EQ(current->zone_map().layout().row_count(), history.size());
          EXPECT_EQ(current->zone_map().layout().capacity(), cap);
          EXPECT_EQ(current->store().amount_totals(), baseline.store().amount_totals());
          EXPECT_TRUE(std::ranges::equal(current->store().timestamps(), baseline.store().timestamps()));
          EXPECT_TRUE(std::ranges::equal(current->store().device_ids(), baseline.store().device_ids()));
          EXPECT_TRUE(std::ranges::equal(current->store().amounts(), baseline.store().amounts()));
        }
        const auto check_query = [&](Query query) {
          const auto expected = reference_evaluate(history, query);
          const ScanOptions options{ScanMode::SelectionVector, 2};
          const auto full = full_scan(table.table(), query);
          EXPECT_EQ(full, full_scan(baseline, query));
          EXPECT_EQ(full.result, expected);
          const auto zoned = zone_map_scan(table.table(), query, options);
          EXPECT_EQ(zoned, zone_map_scan(baseline, query, options));
          EXPECT_EQ(zoned.result, expected);
          const auto automatic = auto_scan(table, query, 2);
          const auto other = auto_scan(rebuilt, query, 2);
          EXPECT_EQ(automatic.execution, other.execution);
          EXPECT_EQ(automatic.plan.strategy, other.plan.strategy);
          EXPECT_EQ(automatic.plan.threads, other.plan.threads);
          EXPECT_EQ(automatic.execution.result, expected);
        };
        for (int id = -6; id <= 6; ++id) {
          std::vector<std::size_t> oracle;
          for (std::size_t i = 0; i < history.size(); ++i) {
            if (history[i].device_id == id && (oracle.empty() || oracle.back() != i / cap))
              oracle.push_back(i / cap);
          }
          const auto postings = table.device_index().candidate_blocks(id);
          ASSERT_TRUE(std::ranges::equal(postings, oracle));
          ASSERT_TRUE(std::ranges::equal(postings, rebuilt.device_index().candidate_blocks(id)));
          const auto indexed = index_scan(table, DeviceEquals{id});
          EXPECT_EQ(indexed, index_scan(rebuilt, DeviceEquals{id}));
          EXPECT_EQ(indexed.result, reference_evaluate(history, Query{DeviceEquals{id}}));
          check_query(Query{DeviceEquals{id}});
        }
        check_query(Query{TimestampRange{-12, 7}});
        check_query(Query{TimestampRange{7, -12}});
      }
    }
  }
}

TEST(Append, CompactOnlyAboveTheThresholdAndPreserveMoveAndCopyContracts) {
  for (const std::size_t initial : std::array<std::size_t, 2>{1, 1024}) {
    const std::vector<Event> base(initial, Event{1, 7, 1});
    IndexedTable table{ZoneMappedTable{base, 1}};
    const auto limit = std::max(std::size_t{64}, initial / 8);
    const std::vector<Event> delta(limit, Event{2, 7, 1});
    table.append(std::span{delta}.first(limit - 1));
    EXPECT_EQ(table.device_index().merge_count(), 0U);
    table.append(std::span{delta}.first(1));
    EXPECT_EQ(table.device_index().merge_count(), 0U);
    IndexedTable copy{table};
    IndexedTable moved{std::move(copy)};
    EXPECT_TRUE(copy.device_index().candidate_blocks(7).empty());
    moved.append(std::span{delta}.first(1));
    EXPECT_EQ(moved.device_index().merge_count(), 1U);
    EXPECT_EQ(moved.device_index().candidate_blocks(7).size(), initial + limit + 1);
    EXPECT_EQ(table.device_index().candidate_blocks(7).size(), initial + limit);
  }
}

TEST(Append, RepeatedDeviceAcrossPartialBlockAndRejectedAmount) {
  for (auto amount : {std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()}) {
    const std::array base{Event{0, 5, amount}};
    IndexedTable table{ZoneMappedTable{base, 3}};
    const std::array more{Event{1, 5, 0}, Event{2, 6, 0}, Event{3, 5, 0}};
    table.append(more);
    const auto view = table.device_index().candidate_blocks(5);
    table.append({});
    const std::array bad{Event{4, 1, amount > 0 ? 1 : -1}};
    EXPECT_THROW(table.append(bad), std::invalid_argument);
    EXPECT_TRUE(std::ranges::equal(view, std::array{0U, 1U}));
    EXPECT_EQ(table.table().store().row_count(), 4U);
    EXPECT_EQ(table.device_index().candidate_blocks(6).size(), 1U);
  }
}
}  // namespace
}  // namespace cqe
