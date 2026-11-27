#include "cqe/zone_mapped_table.hpp"

#include <algorithm>
#include <stdexcept>

#include "append_growth.hpp"
#include "cqe/block_index.hpp"

namespace cqe {

ZoneMappedTable::ZoneMappedTable(std::span<const Event> rows, std::size_t block_capacity)
    : store_(rows), zone_map_(store_, block_capacity) {}

void ZoneMappedTable::append(std::span<const Event> rows) {
  append_impl(rows, nullptr);
}

void ZoneMappedTable::append_impl(std::span<const Event> rows, DeviceBlockIndex* index) {
  if (rows.empty()) return;
  const auto totals = extend_amount_totals(store_.amount_totals_, rows);
  const auto old_count = store_.row_count();
  if (rows.size() > store_.timestamp_.max_size() - old_count) {
    throw std::length_error("append exceeds column size limit");
  }
  const auto capacity = zone_map_.layout_.capacity();
  const BlockLayout layout(old_count + rows.size(), capacity);
  auto timestamps = detail::prepare_growth(store_.timestamp_, rows.size());
  auto devices = detail::prepare_growth(store_.device_id_, rows.size());
  auto amounts = detail::prepare_growth(store_.amount_, rows.size());
  auto zones = detail::prepare_growth(zone_map_.zones_, layout.block_count() - zone_map_.zones_.size());

  // Index publication is the last potentially throwing call. All subsequent scalar writes
  // and vector appends fit in buffers allocated above.
  if (index) index->append(rows, zone_map_.layout_);
  detail::publish_growth(store_.timestamp_, timestamps);
  detail::publish_growth(store_.device_id_, devices);
  detail::publish_growth(store_.amount_, amounts);
  detail::publish_growth(zone_map_.zones_, zones);
  auto offset = old_count % capacity;
  for (const auto& row : rows) {
    store_.timestamp_.push_back(row.timestamp);
    store_.device_id_.push_back(row.device_id);
    store_.amount_.push_back(row.amount);
    if (offset == 0) {
      zone_map_.zones_.push_back({row.timestamp, row.timestamp, row.device_id, row.device_id});
    } else {
      auto& zone = zone_map_.zones_.back();
      zone.timestamp_min = std::min(zone.timestamp_min, row.timestamp);
      zone.timestamp_max = std::max(zone.timestamp_max, row.timestamp);
      zone.device_id_min = std::min(zone.device_id_min, row.device_id);
      zone.device_id_max = std::max(zone.device_id_max, row.device_id);
    }
    if (++offset == capacity) offset = 0;
  }
  store_.amount_totals_ = totals;
  zone_map_.layout_ = layout;
}

}  // namespace cqe
