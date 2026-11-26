#include "cqe/block_index.hpp"

#include <algorithm>
#include <utility>

namespace cqe {
namespace {

using Posting = std::pair<std::int32_t, std::size_t>;

// Given (device ID, block ID) pairs in lexicographic order, retain exactly one copy of
// each distinct pair, keeping the same order and leaving an empty input empty.
void deduplicate_sorted_postings(std::vector<Posting>& postings) {
  const auto unique_end = std::unique(postings.begin(), postings.end());
  postings.erase(unique_end, postings.end());
}

}  // namespace

DeviceBlockIndex::DeviceBlockIndex(const ZoneMappedTable& table) {
  const BlockLayout& layout = table.zone_map().layout();
  const auto devices = table.store().device_ids();
  std::vector<Posting> pairs;
  pairs.reserve(devices.size());
  for (std::size_t block_id = 0; block_id < layout.block_count(); ++block_id) {
    const BlockRange range = layout.block(block_id);
    for (const std::int32_t device_id : devices.subspan(range.begin, range.size())) {
      pairs.emplace_back(device_id, block_id);
    }
  }
  std::sort(pairs.begin(), pairs.end());
  deduplicate_sorted_postings(pairs);

  postings_.reserve(pairs.size());
  for (const auto& [device_id, block_id] : pairs) {
    if (directory_.empty() || directory_.back().device_id != device_id) {
      directory_.push_back(DirectoryEntry{device_id, postings_.size()});
    }
    postings_.push_back(block_id);
  }
}

DeviceBlockIndex::DeviceBlockIndex(DeviceBlockIndex&& other) noexcept
    : directory_(std::move(other.directory_)), postings_(std::move(other.postings_)) {
  other.directory_.clear();
  other.postings_.clear();
}

std::span<const std::size_t> DeviceBlockIndex::candidate_blocks(std::int32_t device_id) const noexcept {
  const auto entry =
      std::lower_bound(directory_.begin(), directory_.end(), device_id,
                       [](const DirectoryEntry& item, std::int32_t value) {
                         return item.device_id < value;
                       });
  if (entry == directory_.end() || entry->device_id != device_id) {
    return {};
  }

  const auto next = entry + 1;
  const std::size_t end = next == directory_.end() ? postings_.size() : next->begin;
  return std::span<const std::size_t>{postings_}.subspan(entry->begin, end - entry->begin);
}

std::size_t DeviceBlockIndex::logical_bytes() const noexcept {
  return directory_.size() * sizeof(DirectoryEntry) + postings_.size() * sizeof(std::size_t);
}

IndexedTable::IndexedTable(ZoneMappedTable table)
    : table_(std::move(table)), device_index_(table_) {}

}  // namespace cqe
