#pragma once

#include <cstdint>

namespace cqe {

// Block skip condition: interval disjointness.
//
// Returns true iff no integer v satisfies both
//
//     zone_min <= v <= zone_max   and   lo <= v <= hi,
//
// that is, iff the closed intervals [zone_min, zone_max] and [lo, hi] have an empty intersection.
//
// - Exact, not only sound: the result is false iff some v lies in both intervals. Returning true
//   means no row of the block can match (no false negative); returning false for a disjoint
//   pair would be a needless scan and is a contract violation here.
// - Closed intervals that meet in a single endpoint intersect. For example, zone [5, 10] and
//   predicate [10, 20] share v = 10, so the result is false and the block is a candidate.
// - lo > hi denotes the empty predicate interval, which matches no value: the result is true for
//   every zone.
// - Precondition: zone_min <= zone_max. A zone describes a non-empty block.
// - Correct for every int64_t input, including INT64_MIN and INT64_MAX, with no signed overflow.
// - noexcept, no allocation, no side effects.
//
// Callers: ZoneMap uses it for a timestamp range [lo, hi] and, with lo == hi == value, for a
// device_id equality, widening the int32_t zone and value to int64_t (lossless).
[[nodiscard]] bool zone_disjoint(std::int64_t zone_min, std::int64_t zone_max, std::int64_t lo,
                                 std::int64_t hi) noexcept;

}  // namespace cqe
