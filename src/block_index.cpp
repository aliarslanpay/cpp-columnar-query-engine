#include "cqe/block_index.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "append_growth.hpp"

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
    : delta_(std::move(other.delta_)), delta_size_(std::exchange(other.delta_size_, 0)),
      merge_count_(std::exchange(other.merge_count_, 0)),
      directory_(std::move(other.directory_)), postings_(std::move(other.postings_)) {
  other.delta_.clear();
  other.directory_.clear();
  other.postings_.clear();
}

std::span<const std::size_t> DeviceBlockIndex::base_blocks(std::int32_t device_id) const noexcept {
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

PostingView DeviceBlockIndex::candidate_blocks(std::int32_t id) const noexcept {
  const auto it = delta_.find(id);
  return {base_blocks(id), it == delta_.end() ? PostingView::Segment{} : PostingView::Segment{it->second}};
}

void DeviceBlockIndex::append(std::span<const Event> rows, const BlockLayout& old_layout) {
  Delta additions;
  auto next_size = delta_size_;
  auto row_number = old_layout.row_count();
  for (const auto& row : rows) {
    const auto block = row_number++ / old_layout.capacity();
    auto& tail = additions[row.device_id];
    if (!tail.empty()) {
      if (tail.back() == block) continue;
    } else {
      const auto prior = delta_.find(row.device_id);
      const auto previous = prior == delta_.end() ? base_blocks(row.device_id)
                                                   : PostingView::Segment{prior->second};
      if (!previous.empty() && previous.back() == block) continue;
    }
    tail.push_back(block);
    ++next_size;
  }

  if (next_size > std::max(std::size_t{64}, postings_.size() / 8)) {
    compact(additions);
    return;
  }

  struct Update {
    std::vector<std::size_t>& live;
    std::vector<std::size_t> growth;
    std::vector<std::size_t> suffix;
  };
  std::vector<Update> updates;
  for (auto it = additions.begin(); it != additions.end();) {
    if (it->second.empty()) {
      it = additions.erase(it);
      continue;
    }
    const auto prior = delta_.find(it->first);
    if (prior == delta_.end()) {
      ++it;  // Already a complete new node.
    } else {
      auto growth = detail::prepare_growth(prior->second, it->second.size());
      updates.push_back({prior->second, std::move(growth), std::move(it->second)});
      it = additions.erase(it);
    }
  }

  for (auto& update : updates) {
    detail::publish_growth(update.live, update.growth);
    update.live.insert(update.live.end(), update.suffix.begin(), update.suffix.end());
  }
  delta_.merge(additions);  // Identical allocators, nonthrowing integer comparison.
  delta_size_ = next_size;
}

void DeviceBlockIndex::compact(const Delta& additions) {
  std::vector<DirectoryEntry> directory;
  std::vector<std::size_t> postings;
  auto base = directory_.begin();
  auto delta = delta_.begin();
  auto added = additions.begin();
  // Walk the three sorted directories together. Within one device all nonempty segments
  // are disjoint and already in chronological block order.
  while (base != directory_.end() || delta != delta_.end() || added != additions.end()) {
    auto id = std::numeric_limits<std::int32_t>::max();
    if (base != directory_.end()) id = std::min(id, base->device_id);
    if (delta != delta_.end()) id = std::min(id, delta->first);
    if (added != additions.end()) id = std::min(id, added->first);
    const auto begin = postings.size();
    if (base != directory_.end() && base->device_id == id) {
      const auto first = base->begin;
      ++base;
      const auto end = base == directory_.end() ? postings_.size() : base->begin;
      const auto segment = std::span{postings_}.subspan(first, end - first);
      postings.insert(postings.end(), segment.begin(), segment.end());
    }
    if (delta != delta_.end() && delta->first == id) {
      postings.insert(postings.end(), delta->second.begin(), delta->second.end());
      ++delta;
    }
    if (added != additions.end() && added->first == id) {
      postings.insert(postings.end(), added->second.begin(), added->second.end());
      ++added;
    }
    if (postings.size() != begin) directory.push_back({id, begin});
  }
  postings_.swap(postings);
  directory_.swap(directory);
  delta_.clear();
  delta_size_ = 0;
  ++merge_count_;
}

std::size_t DeviceBlockIndex::logical_bytes() const noexcept {
  return directory_.size() * sizeof(DirectoryEntry) + (postings_.size() + delta_size_) * sizeof(std::size_t) +
         delta_.size() * sizeof(std::int32_t);
}

IndexedTable::IndexedTable(ZoneMappedTable table)
    : table_(std::move(table)), device_index_(table_) {}

void IndexedTable::append(std::span<const Event> rows) {
  table_.append_impl(rows, &device_index_);
}

}  // namespace cqe
