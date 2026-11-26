#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cqe/zone_mapped_table.hpp"

namespace cqe {

// Immutable device postings. An IndexedTable builds them from its own store and block layout.
class DeviceBlockIndex {
 public:
  // Exact, strictly increasing block IDs for this device, or an empty span if it is absent.
  // The span refers to the index's storage and is valid until the index is moved or destroyed.
  [[nodiscard]] std::span<const std::size_t> candidate_blocks(std::int32_t device_id) const noexcept;

  // Bytes occupied by the directory and postings entries, including any entry padding.
  // Excludes vector objects, unused capacity and temporary build storage.
  [[nodiscard]] std::size_t logical_bytes() const noexcept;

 private:
  friend class IndexedTable;

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
