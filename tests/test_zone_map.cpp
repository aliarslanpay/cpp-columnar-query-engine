#include "cqe/zone_map.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "cqe/column_store.hpp"
#include "cqe/dataset.hpp"
#include "cqe/zone_mapped_table.hpp"
#include "test_util.hpp"
#include "zone_test_util.hpp"

namespace cqe {
namespace {

using test::kInt32Max;
using test::kInt32Min;
using test::kInt64Max;
using test::kInt64Min;

// Expected zones computed directly from the rows: row i belongs to block i / capacity. This uses
// neither BlockLayout nor ZoneMap.
std::vector<BlockZone> brute_force_zones(const std::vector<Event>& rows, std::size_t capacity) {
  std::vector<BlockZone> zones;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const Event& row = rows[i];
    if (i % capacity == 0) {
      zones.push_back(BlockZone{row.timestamp, row.timestamp, row.device_id, row.device_id});
      continue;
    }
    BlockZone& zone = zones.back();
    zone.timestamp_min = std::min(zone.timestamp_min, row.timestamp);
    zone.timestamp_max = std::max(zone.timestamp_max, row.timestamp);
    zone.device_id_min = std::min(zone.device_id_min, row.device_id);
    zone.device_id_max = std::max(zone.device_id_max, row.device_id);
  }
  return zones;
}

std::vector<BlockZone> zones_of(const ZoneMap& zone_map) {
  return {zone_map.zones().begin(), zone_map.zones().end()};
}

// --- ZoneMap ------------------------------------------------------------------------------------

TEST(ZoneMap, RejectsZeroCapacity) {
  const std::vector<Event> rows = {{1, 1, 1}};
  const ColumnStore store{rows};
  EXPECT_THROW(ZoneMap(store, 0), std::invalid_argument);
}

TEST(ZoneMap, EmptyStoreHasNoZones) {
  const std::vector<Event> rows;
  const ZoneMap zone_map(ColumnStore{rows}, 4);
  EXPECT_EQ(zone_map.layout().row_count(), 0U);
  EXPECT_EQ(zone_map.layout().block_count(), 0U);
  EXPECT_TRUE(zone_map.zones().empty());
  EXPECT_TRUE(zone_map.candidate_blocks(TimestampRange{kInt64Min, kInt64Max}).empty());
  EXPECT_TRUE(zone_map.candidate_blocks(DeviceEquals{0}).empty());
}

TEST(ZoneMap, ZonesAreTheExactMinAndMaxOfEachBlock) {
  constexpr std::array<std::size_t, 5> kRowCounts = {1, 1023, 1024, 1025, 5000};
  constexpr std::array<std::size_t, 4> kCapacities = {1, 3, 1024, 4096};
  for (const DeviceDistribution distribution : {DeviceDistribution::Uniform, DeviceDistribution::Zipf}) {
    for (const Ordering ordering : {Ordering::TimeSorted, Ordering::Shuffled, Ordering::DeviceSorted}) {
      for (const std::size_t n : kRowCounts) {
        GeneratorConfig config;
        config.row_count = n;
        config.seed = 11;
        config.device_distribution = distribution;
        const std::vector<Event> rows = generate_events(config, ordering);
        const ColumnStore store{rows};
        for (const std::size_t capacity : kCapacities) {
          const ZoneMap zone_map(store, capacity);
          EXPECT_EQ(zone_map.layout().row_count(), n);
          EXPECT_EQ(zone_map.layout().capacity(), capacity);
          EXPECT_EQ(zones_of(zone_map), brute_force_zones(rows, capacity))
              << to_string(ordering) << " " << to_string(distribution) << " n " << n
              << " capacity " << capacity;
        }
      }
    }
  }
}

TEST(ZoneMap, PartialLastBlockUsesOnlyItsOwnRows) {
  // Capacity 2: blocks {0, 1}, {2, 3} and the partial block {4}.
  const std::vector<Event> rows = {{50, 5, 0}, {10, 1, 0}, {40, 4, 0}, {20, 2, 0}, {30, 3, 0}};
  const ZoneMap zone_map(ColumnStore{rows}, 2);
  ASSERT_EQ(zone_map.zones().size(), 3U);
  EXPECT_EQ(zone_map.zones()[0], (BlockZone{10, 50, 1, 5}));
  EXPECT_EQ(zone_map.zones()[1], (BlockZone{20, 40, 2, 4}));
  EXPECT_EQ(zone_map.zones()[2], (BlockZone{30, 30, 3, 3}));
}

TEST(ZoneMap, ExtremeValuesAreKeptExactly) {
  const std::vector<Event> rows = {
      {kInt64Min, kInt32Max, 1}, {kInt64Max, kInt32Min, 2}, {0, 0, 3}, {kInt64Max, kInt32Max, 4}};
  const ZoneMap zone_map(ColumnStore{rows}, 2);
  ASSERT_EQ(zone_map.zones().size(), 2U);
  EXPECT_EQ(zone_map.zones()[0], (BlockZone{kInt64Min, kInt64Max, kInt32Min, kInt32Max}));
  EXPECT_EQ(zone_map.zones()[1], (BlockZone{0, kInt64Max, 0, kInt32Max}));
}

TEST(ZoneMap, CandidateBlocksAreAscendingUniqueAndInRange) {
  GeneratorConfig config;
  config.row_count = 5000;
  config.seed = 4;
  config.device_count = 50;
  for (const Ordering ordering : {Ordering::TimeSorted, Ordering::Shuffled, Ordering::DeviceSorted}) {
    const std::vector<Event> rows = generate_events(config, ordering);
    const ZoneMap zone_map(ColumnStore{rows}, 7);
    for (const Query& query : test::random_queries(rows, config.device_count, 99, 200)) {
      test::expect_well_formed_candidates(zone_map.candidate_blocks(query.predicate),
                                          zone_map.layout().block_count());
    }
  }
}

TEST(ZoneMap, OnlyInvariantPreservingSpecialMembersAreProvided) {
  static_assert(std::is_copy_constructible_v<ZoneMap>);
  static_assert(std::is_nothrow_move_constructible_v<ZoneMap>);
  static_assert(!std::is_copy_assignable_v<ZoneMap>);
  static_assert(!std::is_move_assignable_v<ZoneMap>);
  SUCCEED();
}

TEST(ZoneMap, MoveLeavesTheSourceAsTheEmptyZoneMap) {
  const std::vector<Event> rows = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
  ZoneMap source(ColumnStore{rows}, 2);
  const ZoneMap target(std::move(source));
  EXPECT_EQ(target.layout().row_count(), 3U);
  EXPECT_EQ(zones_of(target), brute_force_zones(rows, 2));
  // Inspecting the moved-from state is the point of this test.
  EXPECT_EQ(source.layout().row_count(), 0U);
  EXPECT_EQ(source.layout().capacity(), 2U);
  EXPECT_EQ(source.layout().block_count(), 0U);
  EXPECT_TRUE(source.zones().empty());
  EXPECT_TRUE(source.candidate_blocks(TimestampRange{kInt64Min, kInt64Max}).empty());
}

TEST(ZoneMap, CopyIsEqualAndIndependent) {
  const std::vector<Event> rows = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
  const ZoneMap original(ColumnStore{rows}, 2);
  const ZoneMap copy(original);
  EXPECT_EQ(zones_of(copy), zones_of(original));
  EXPECT_EQ(copy.layout().row_count(), original.layout().row_count());
  EXPECT_NE(copy.zones().data(), original.zones().data());
}

// --- ZoneMappedTable ----------------------------------------------------------------------------

TEST(ZoneMappedTable, BuildsTheZoneMapFromItsOwnStore) {
  GeneratorConfig config;
  config.row_count = 3000;
  config.seed = 8;
  const std::vector<Event> rows = generate_events(config, Ordering::Shuffled);
  const ZoneMappedTable table{rows, 256};
  EXPECT_EQ(table.store().row_count(), rows.size());
  EXPECT_EQ(table.zone_map().layout().row_count(), rows.size());
  EXPECT_EQ(table.zone_map().layout().capacity(), 256U);
  EXPECT_EQ(zones_of(table.zone_map()), brute_force_zones(rows, 256));
}

TEST(ZoneMappedTable, DefaultCapacityIs1024) {
  const std::vector<Event> rows(3000, Event{1, 1, 1});
  const ZoneMappedTable table{rows};
  EXPECT_EQ(table.zone_map().layout().capacity(), 1024U);
  EXPECT_EQ(table.zone_map().layout().block_count(), 3U);
}

TEST(ZoneMappedTable, RejectsZeroCapacityAndInvariantViolations) {
  const std::vector<Event> rows = {{1, 1, 1}};
  EXPECT_THROW(ZoneMappedTable(rows, 0), std::invalid_argument);
  const std::vector<Event> too_positive = {{0, 0, kInt64Max}, {1, 0, 1}};
  EXPECT_THROW(ZoneMappedTable(too_positive, 4), std::invalid_argument);
}

TEST(ZoneMappedTable, OnlyConsistencyPreservingSpecialMembersAreProvided) {
  static_assert(std::is_copy_constructible_v<ZoneMappedTable>);
  static_assert(std::is_nothrow_move_constructible_v<ZoneMappedTable>);
  static_assert(!std::is_copy_assignable_v<ZoneMappedTable>);
  static_assert(!std::is_move_assignable_v<ZoneMappedTable>);
  SUCCEED();
}

TEST(ZoneMappedTable, MoveLeavesTheSourceAsTheEmptyTableWithTheEmptyZoneMap) {
  const std::vector<Event> rows = {{1, 1, 5}, {2, 2, -3}, {3, 3, 4}};
  ZoneMappedTable source{rows, 2};
  const ZoneMappedTable target{std::move(source)};
  EXPECT_EQ(target.store().row_count(), 3U);
  EXPECT_EQ(zones_of(target.zone_map()), brute_force_zones(rows, 2));
  // Inspecting the moved-from state is the point of this test.
  EXPECT_EQ(source.store().row_count(), 0U);
  EXPECT_EQ(source.store().amount_totals(), (AmountTotals{0, 0}));
  EXPECT_EQ(source.zone_map().layout().row_count(), 0U);
  EXPECT_TRUE(source.zone_map().zones().empty());
}

}  // namespace
}  // namespace cqe
