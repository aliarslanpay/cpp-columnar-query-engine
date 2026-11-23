#include "cqe/block_skip.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cqe {
namespace {

constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

struct EdgeCase {
  std::int64_t zone_min;
  std::int64_t zone_max;
  std::int64_t lo;
  std::int64_t hi;
  bool disjoint;  // expected result, worked out by hand from the contract
  const char* what;
};

// Hand-written edge cases with explicit expected results (contract in include/cqe/block_skip.hpp).
constexpr EdgeCase kEdgeCases[] = {
    // Disjoint on either side, and touching in a single endpoint.
    {10, 20, 0, 5, true, "predicate entirely below the zone"},
    {10, 20, 0, 9, true, "predicate ends one below zone_min"},
    {10, 20, 0, 10, false, "predicate ends at zone_min"},
    {10, 20, 25, 30, true, "predicate entirely above the zone"},
    {10, 20, 21, 30, true, "predicate starts one above zone_max"},
    {10, 20, 20, 30, false, "predicate starts at zone_max"},
    {-20, -10, -9, -5, true, "negative values, predicate one above zone_max"},
    {-20, -10, -10, -5, false, "negative values, predicate starts at zone_max"},

    // Overlapping and nested intervals.
    {10, 20, 5, 15, false, "predicate overlaps the lower part of the zone"},
    {10, 20, 15, 25, false, "predicate overlaps the upper part of the zone"},
    {10, 20, 12, 18, false, "predicate inside the zone"},
    {10, 20, 0, 30, false, "zone inside the predicate"},
    {10, 20, 10, 20, false, "equal intervals"},

    // Single-point predicates.
    {10, 20, 10, 10, false, "point predicate at zone_min"},
    {10, 20, 20, 20, false, "point predicate at zone_max"},
    {10, 20, 15, 15, false, "point predicate inside the zone"},
    {10, 20, 9, 9, true, "point predicate one below zone_min"},
    {10, 20, 21, 21, true, "point predicate one above zone_max"},

    // Single-point zones.
    {7, 7, 7, 7, false, "point zone equal to point predicate"},
    {7, 7, 6, 6, true, "point zone, point predicate one below"},
    {7, 7, 8, 8, true, "point zone, point predicate one above"},
    {7, 7, 0, 7, false, "predicate ends at the point zone"},
    {7, 7, 7, 9, false, "predicate starts at the point zone"},
    {7, 7, 0, 6, true, "predicate ends one below the point zone"},
    {7, 7, 8, 9, true, "predicate starts one above the point zone"},

    // Inverted predicates (lo > hi) are empty and disjoint from every zone.
    {10, 20, 15, 14, true, "inverted predicate inside the zone"},
    {10, 20, 11, 10, true, "inverted predicate with lo == hi + 1 at zone_min"},
    {10, 20, 20, 10, true, "inverted predicate equal to the zone reversed"},
    {10, 20, 21, 9, true, "inverted predicate whose endpoints straddle the zone"},
    {7, 7, 8, 7, true, "inverted predicate around a point zone"},
    {10, 20, kMax, kMin, true, "inverted predicate over the whole int64 range"},

    // INT64_MIN and INT64_MAX.
    {kMin, kMax, kMin, kMax, false, "full zone, full predicate"},
    {kMin, kMax, 0, 0, false, "full zone, point predicate inside"},
    {kMin, kMax, kMin, kMin, false, "full zone, point predicate at INT64_MIN"},
    {kMin, kMax, kMax, kMax, false, "full zone, point predicate at INT64_MAX"},
    {kMin, kMin, kMin, kMin, false, "point zone and point predicate at INT64_MIN"},
    {kMax, kMax, kMax, kMax, false, "point zone and point predicate at INT64_MAX"},
    {kMin, kMin, kMin, kMax, false, "full predicate covers a point zone at INT64_MIN"},
    {kMax, kMax, kMin, kMax, false, "full predicate covers a point zone at INT64_MAX"},
    {kMin, kMin, kMin + 1, kMax, true, "zone at INT64_MIN, predicate starts one above"},
    {kMax, kMax, kMin, kMax - 1, true, "zone at INT64_MAX, predicate ends one below"},
    {kMin, kMax - 1, kMax, kMax, true, "point predicate at INT64_MAX above the zone"},
    {kMin + 1, kMax, kMin, kMin, true, "point predicate at INT64_MIN below the zone"},
    {kMin, 0, 0, kMax, false, "lower and upper halves touching at 0"},
    {kMin, -1, 0, kMax, true, "adjacent halves, zone below"},
    {0, kMax, kMin, -1, true, "adjacent halves, zone above"},
    {kMin, kMax, kMax, kMin, true, "inverted full range against the full zone"},
    {kMin, kMax, kMin + 1, kMin, true, "inverted predicate with lo == hi + 1 at INT64_MIN"},
    {kMin, kMax, kMax, kMax - 1, true, "inverted predicate with lo == hi + 1 at INT64_MAX"},
    {kMax, kMax, kMax, kMin, true, "inverted predicate against a point zone at INT64_MAX"},
};

TEST(ZoneDisjoint, HandWrittenEdgeCases) {
  for (const EdgeCase& c : kEdgeCases) {
    EXPECT_EQ(zone_disjoint(c.zone_min, c.zone_max, c.lo, c.hi), c.disjoint)
        << c.what << ": zone [" << c.zone_min << ", " << c.zone_max << "], predicate [" << c.lo
        << ", " << c.hi << "]";
  }
}

// Expected value by enumerating every v of the zone; independent of any closed-form condition.
bool disjoint_by_enumeration(std::int64_t zone_min, std::int64_t zone_max, std::int64_t lo,
                             std::int64_t hi) {
  for (std::int64_t v = zone_min; v <= zone_max; ++v) {
    if (lo <= v && v <= hi) {
      return false;
    }
  }
  return true;
}

// Every zone [zone_min, zone_max] with zone_min <= zone_max and every predicate [lo, hi] with all
// endpoints in [-6, 6]: 91 zones x 169 predicates = 15379 cases. The predicates include inverted
// ones (lo > hi) and ones that touch the zone in a single endpoint.
TEST(ZoneDisjoint, MatchesExhaustiveEnumerationOnASmallDomain) {
  constexpr std::int64_t kLow = -6;
  constexpr std::int64_t kHigh = 6;
  std::size_t cases = 0;
  std::size_t mismatches = 0;
  for (std::int64_t zone_min = kLow; zone_min <= kHigh; ++zone_min) {
    for (std::int64_t zone_max = zone_min; zone_max <= kHigh; ++zone_max) {
      for (std::int64_t lo = kLow; lo <= kHigh; ++lo) {
        for (std::int64_t hi = kLow; hi <= kHigh; ++hi) {
          ++cases;
          const bool expected = disjoint_by_enumeration(zone_min, zone_max, lo, hi);
          const bool actual = zone_disjoint(zone_min, zone_max, lo, hi);
          if (actual != expected) {
            ++mismatches;
            if (mismatches <= 10) {
              ADD_FAILURE() << "zone [" << zone_min << ", " << zone_max << "], predicate [" << lo
                            << ", " << hi << "]: expected " << expected << ", got " << actual;
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 15379U);
  EXPECT_EQ(mismatches, 0U) << "only the first 10 mismatches are listed above";
}

}  // namespace
}  // namespace cqe
