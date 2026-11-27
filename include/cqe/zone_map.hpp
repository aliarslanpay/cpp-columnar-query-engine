#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cqe/block_layout.hpp"
#include "cqe/column_store.hpp"
#include "cqe/types.hpp"

namespace cqe {

// Exact minimum and maximum of the timestamp and device_id columns over one block's rows.
struct BlockZone {
  std::int64_t timestamp_min = 0;
  std::int64_t timestamp_max = 0;
  std::int32_t device_id_min = 0;
  std::int32_t device_id_max = 0;

  friend bool operator==(const BlockZone&, const BlockZone&) = default;
};

// Zone map: per-block metadata that only produces candidate blocks. It never reads rows while
// answering a predicate; the executor applies the exact predicate to the candidate blocks.
//
// Invariant: zones().size() == layout().block_count(), and zones()[b] holds the exact (attained)
// minimum and maximum of block b's rows. For the partial last block only its actual rows count.
// The empty table has no blocks and no zones. No zone is empty, so no sentinel values are used.
//
// Lifecycle: a ZoneMap copies what it needs and holds no pointer or reference to the store it
// was built from. The owning table updates it when rows are appended. Queries do not take a
// store and a zone map as two independent arguments; they go through ZoneMappedTable, which owns
// a store and the zone map built from it (zone_mapped_table.hpp). Concurrent readers require
// that no append is in progress.
class ZoneMap {
 public:
  // Throws std::invalid_argument if block_capacity == 0.
  ZoneMap(const ColumnStore& store, std::size_t block_capacity);

  [[nodiscard]] const BlockLayout& layout() const noexcept { return layout_; }
  [[nodiscard]] std::span<const BlockZone> zones() const noexcept { return zones_; }

  // Blocks that may contain a row satisfying `predicate`, in ascending order without duplicates:
  // every block whose zone is not disjoint from the predicate interval (block_skip.hpp). A
  // timestamp range uses [lo, hi]; a device_id equality uses [value, value].
  [[nodiscard]] std::vector<std::size_t> candidate_blocks(const Predicate& predicate) const;

  // Special members keep the invariant at all times (same rules as the stores).
  // - Copy construction is all-or-nothing.
  // - Move construction leaves the source as the zone map of the empty table (row count 0, no
  //   zones, same capacity). The implicit one would keep the old layout next to emptied zones.
  // - Assignment is not provided.
  ZoneMap(const ZoneMap&) = default;
  ZoneMap(ZoneMap&& other) noexcept;
  ZoneMap& operator=(const ZoneMap&) = delete;
  ZoneMap& operator=(ZoneMap&&) = delete;
  ~ZoneMap() = default;

 private:
  friend class ZoneMappedTable;
  BlockLayout layout_;
  std::vector<BlockZone> zones_;
};

}  // namespace cqe
