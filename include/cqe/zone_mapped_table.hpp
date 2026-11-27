#pragma once

#include <cstddef>
#include <span>

#include "cqe/block_layout.hpp"
#include "cqe/column_store.hpp"
#include "cqe/types.hpp"
#include "cqe/zone_map.hpp"

namespace cqe {

class DeviceBlockIndex;

// A column store together with the zone map built from it: the unit a zone-map query runs on.
//
// The table owns its store and builds the zone map from that store in its constructor, so the
// metadata always describes exactly the rows it is used with. Passing a store and a zone map as
// two independent arguments could not guarantee this: a row-count check, for example, accepts
// the zone map of a one-row table with timestamp 10 for a one-row table with timestamp 100, and
// a query for [100, 100] would then skip the only matching block.
class ZoneMappedTable {
 public:
  // Copies `rows` into the owned column store, then builds its zone map.
  // Throws std::invalid_argument if the rows violate the amount-total invariant (sum_bound.hpp)
  // or if block_capacity == 0.
  explicit ZoneMappedTable(std::span<const Event> rows,
                           std::size_t block_capacity = BlockLayout::kDefaultCapacity);

  // Exclusive operation. Nonempty success invalidates borrowed views. Empty input and
  // exceptions preserve all rows, metadata and outstanding views. After success, columns,
  // amount totals, layout and block extrema match construction from the concatenated rows.
  void append(std::span<const Event> rows);

  // Read-only access, for example to run the full-scan baseline on the same data.
  [[nodiscard]] const ColumnStore& store() const noexcept { return store_; }
  [[nodiscard]] const ZoneMap& zone_map() const noexcept { return zone_map_; }

  // Special members keep the store and the zone map in agreement.
  // - Copy construction copies both (all-or-nothing).
  // - Move construction moves both; each member's move constructor leaves its source empty, so
  //   the moved-from table is the empty table with the empty zone map.
  // - Assignment is not provided (neither member has one).
  ZoneMappedTable(const ZoneMappedTable&) = default;
  ZoneMappedTable(ZoneMappedTable&&) noexcept = default;
  ZoneMappedTable& operator=(const ZoneMappedTable&) = delete;
  ZoneMappedTable& operator=(ZoneMappedTable&&) = delete;
  ~ZoneMappedTable() = default;

 private:
  friend class IndexedTable;
  void append_impl(std::span<const Event> rows, DeviceBlockIndex* index);
  ColumnStore store_;  // declared first: zone_map_ is built from it
  ZoneMap zone_map_;
};

}  // namespace cqe
