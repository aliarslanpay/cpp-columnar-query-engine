#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "cqe/posting_view.hpp"
#include "cqe/zone_mapped_table.hpp"

namespace cqe {

// For each device the base followed by the delta lists exactly its occupied blocks, in
// strictly increasing order. Lookups borrow both segments. When delta postings exceed
// max(64, base postings / 8), compaction streams both into a new base in linear time
// and temporary space. Ordinary appends only update devices in the batch.
class DeviceBlockIndex {
 public:
  // Exact, strictly increasing block IDs for this device, or an empty view if it is absent.
  // The view expires on successful nonempty append, move or destruction.
  [[nodiscard]] PostingView candidate_blocks(std::int32_t device_id) const noexcept;

  // Base directory entries (including padding), delta device keys and all block IDs.
  // Excludes container objects, tree-node overhead, spare capacity and temporary storage.
  [[nodiscard]] std::size_t logical_bytes() const noexcept;

  [[nodiscard]] std::size_t merge_count() const noexcept { return merge_count_; }

 private:
  friend class ZoneMappedTable;
  friend class IndexedTable;
  using Delta = std::map<std::int32_t, std::vector<std::size_t>>;
  std::span<const std::size_t> base_blocks(std::int32_t id) const noexcept;
  void append(std::span<const Event> rows, const BlockLayout& old_layout);
  void compact(const Delta& additions);
  Delta delta_;
  std::size_t delta_size_ = 0;
  std::size_t merge_count_ = 0;

  struct DirectoryEntry {
    std::int32_t device_id;
    std::size_t begin;
  };

  explicit DeviceBlockIndex(const ZoneMappedTable& table);
  DeviceBlockIndex(const DeviceBlockIndex&) = default;
  DeviceBlockIndex(DeviceBlockIndex&& other) noexcept;

  std::vector<DirectoryEntry> directory_;
  std::vector<std::size_t> postings_;
};

// Owns the indexed rows and all metadata together. Passing a prebuilt table by move allows
// the cost of building its index to be measured separately.
class IndexedTable {
 public:
  explicit IndexedTable(ZoneMappedTable table);

  // Same exclusivity, view lifetime and strong exception guarantee as ZoneMappedTable::append.
  void append(std::span<const Event> rows);

  IndexedTable(const IndexedTable&) = default;
  // The moved-from object is an empty table with empty metadata.
  IndexedTable(IndexedTable&&) noexcept = default;
  IndexedTable& operator=(const IndexedTable&) = delete;
  IndexedTable& operator=(IndexedTable&&) = delete;

  [[nodiscard]] const ZoneMappedTable& table() const noexcept { return table_; }
  [[nodiscard]] const DeviceBlockIndex& device_index() const noexcept { return device_index_; }

 private:
  ZoneMappedTable table_;
  DeviceBlockIndex device_index_;
};

}  // namespace cqe
