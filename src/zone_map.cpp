#include "cqe/zone_map.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

#include "cqe/block_skip.hpp"

namespace cqe {

ZoneMap::ZoneMap(const ColumnStore& store, std::size_t block_capacity)
    : layout_(store.row_count(), block_capacity) {
  const std::span<const std::int64_t> timestamps = store.timestamps();
  const std::span<const std::int32_t> device_ids = store.device_ids();
  zones_.reserve(layout_.block_count());
  for (std::size_t b = 0; b < layout_.block_count(); ++b) {
    const BlockRange range = layout_.block(b);
    // Non-empty by BlockLayout's guarantee, so minmax_element never returns end().
    const auto ts = timestamps.subspan(range.begin, range.size());
    const auto dev = device_ids.subspan(range.begin, range.size());
    const auto [ts_min, ts_max] = std::minmax_element(ts.begin(), ts.end());
    const auto [dev_min, dev_max] = std::minmax_element(dev.begin(), dev.end());
    zones_.push_back(BlockZone{*ts_min, *ts_max, *dev_min, *dev_max});
  }
}

ZoneMap::ZoneMap(ZoneMap&& other) noexcept
    : layout_(other.layout_), zones_(std::move(other.zones_)) {
  // Make the source the zone map of the empty table. A moved-from vector is only "valid but
  // unspecified", so clear it explicitly. BlockLayout(0, capacity) cannot throw here because the
  // source's capacity is already >= 1.
  other.zones_.clear();
  other.layout_ = BlockLayout(0, other.layout_.capacity());
}

std::vector<std::size_t> ZoneMap::candidate_blocks(const Predicate& predicate) const {
  std::vector<std::size_t> candidates;
  std::visit(
      [&](const auto& p) {
        using P = std::decay_t<decltype(p)>;
        for (std::size_t b = 0; b < zones_.size(); ++b) {
          const BlockZone& zone = zones_[b];
          bool disjoint = false;
          if constexpr (std::is_same_v<P, TimestampRange>) {
            disjoint = zone_disjoint(zone.timestamp_min, zone.timestamp_max, p.lo, p.hi);
          } else {
            static_assert(std::is_same_v<P, DeviceEquals>);
            disjoint = zone_disjoint(zone.device_id_min, zone.device_id_max, p.value, p.value);
          }
          if (!disjoint) {
            candidates.push_back(b);
          }
        }
      },
      predicate);
  return candidates;
}

}  // namespace cqe
