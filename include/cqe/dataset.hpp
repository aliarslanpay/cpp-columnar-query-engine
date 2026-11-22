#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "cqe/types.hpp"

namespace cqe {

// Row orderings over the same multiset of rows.
enum class Ordering {
  TimeSorted,    // generation order: timestamps non-decreasing
  Shuffled,      // a uniformly random permutation of TimeSorted
  DeviceSorted,  // sorted by (device_id, timestamp)
};

// Distribution of device_id over [0, device_count).
enum class DeviceDistribution {
  Uniform,
  Zipf,  // P(device_id == k) proportional to 1 / (k + 1)^zipf_exponent; id 0 is the most frequent
};

[[nodiscard]] std::string_view to_string(Ordering ordering);
[[nodiscard]] std::string_view to_string(DeviceDistribution distribution);

struct GeneratorConfig {
  std::size_t row_count = 0;
  std::uint64_t seed = 1;
  std::int32_t device_count = 1000;  // device ids are in [0, device_count)
  DeviceDistribution device_distribution = DeviceDistribution::Uniform;
  double zipf_exponent = 1.0;  // used only by DeviceDistribution::Zipf
  std::int64_t timestamp_start = 1'700'000'000'000;  // timestamp of the first row
  std::int64_t max_timestamp_step = 3;  // consecutive timestamps differ by a step in [0, max]
  std::int64_t amount_min = -1'000'000;
  std::int64_t amount_max = 1'000'000;
};

// Generates the `events` table in the requested ordering.
//
// Rows. Row i (in time order) has timestamp_start plus the sum of i random steps in
// [0, max_timestamp_step], so equal timestamps occur when a step of 0 is drawn. Its device_id
// follows `device_distribution`, and its amount is uniform in [amount_min, amount_max].
//
// Orderings. The rows are always drawn first, in time order, from one random stream; the
// ordering is applied afterwards. For a fixed config the three orderings therefore contain the
// same multiset of rows.
//   - TimeSorted: the generation order.
//   - Shuffled: Fisher-Yates permutation of the generation order, from the same stream.
//   - DeviceSorted: stable sort of the generation order by device_id, i.e. by (device_id, timestamp).
//
// Determinism scope. With the documented toolchain (Clang 18, libstdc++ 13, glibc 2.39, x86-64
// Linux, no -ffast-math) the result is a pure function of (config, ordering): identical across
// runs, processes and build presets, which the tests pin with golden fingerprints. The generator
// uses std::mt19937_64 (output sequence fixed by the standard), its own unbiased integer sampling
// and its own shuffle (std::uniform_int_distribution and std::shuffle are implementation-defined).
// The Uniform path is integer-only. The Zipf path builds its CDF with floating point (std::pow,
// division), so another compiler, standard library, libm or set of flags may produce different
// rows. No bit-level reproducibility is claimed outside the documented toolchain, nor across
// versions of this code.
//
// Validation. Throws std::invalid_argument, before generating anything, if
//   - device_count < 1, amount_min > amount_max or max_timestamp_step < 0;
//   - Zipf is selected and zipf_exponent is not finite and > 0;
//   - the last timestamp, timestamp_start + (row_count - 1) * max_timestamp_step, could exceed
//     INT64_MAX;
//   - row_count > 0 and max(|amount_min|, |amount_max|) > INT64_MAX / row_count.
// The last condition is a sufficient, conservative bound: it implies the amount-total invariant
// (sum_bound.hpp) for every table the generator can produce. It may reject configurations whose
// tables would still satisfy the invariant (for example four rows of INT64_MIN / 4).
[[nodiscard]] std::vector<Event> generate_events(const GeneratorConfig& config, Ordering ordering);

}  // namespace cqe
