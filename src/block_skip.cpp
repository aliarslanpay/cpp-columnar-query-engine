#include "cqe/block_skip.hpp"

namespace cqe {

bool zone_disjoint(std::int64_t zone_min, std::int64_t zone_max, std::int64_t lo,
                   std::int64_t hi) noexcept {
  // An inverted predicate interval (lo > hi) contains no value, so it shares none with any zone.
  if (lo > hi) {
    return true;
  }
  // Both intervals are now non-empty (zone_min <= zone_max is the precondition). Two non-empty
  // closed intervals intersect iff each starts no later than the other ends, so they are disjoint
  // iff the predicate ends before the zone starts or the zone ends before the predicate starts.
  // Equal endpoints count as intersecting. Only comparisons are used, so no input can overflow.
  return hi < zone_min || zone_max < lo;
}

}  // namespace cqe
