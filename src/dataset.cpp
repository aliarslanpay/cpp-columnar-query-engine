#include "cqe/dataset.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cqe {
namespace {

using Rng = std::mt19937_64;
static_assert(Rng::min() == 0 && Rng::max() == std::numeric_limits<std::uint64_t>::max(),
              "the sampling helpers assume a full-range 64-bit engine");

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

// Unbiased integer in [0, bound), bound > 0, by rejection sampling.
std::uint64_t uniform_below(Rng& rng, std::uint64_t bound) {
  // threshold = 2^64 mod bound. Accepting only draws >= threshold leaves a number of candidates
  // that is a multiple of `bound`, so `draw % bound` is exactly uniform.
  const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
  for (;;) {
    const std::uint64_t draw = rng();
    if (draw >= threshold) {
      return draw % bound;
    }
  }
}

// Uniform integer in [lo, hi], lo <= hi; the full int64_t range is allowed.
std::int64_t uniform_inclusive(Rng& rng, std::int64_t lo, std::int64_t hi) {
  // hi - lo computed modulo 2^64 is exact because 0 <= hi - lo <= 2^64 - 1.
  const std::uint64_t span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
  const std::uint64_t offset =
      span == std::numeric_limits<std::uint64_t>::max() ? rng() : uniform_below(rng, span + 1);
  // C++20 defines unsigned-to-signed conversion as modular, so this is lo + offset exactly.
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + offset);
}

// Uniform double in [0, 1) from the top 53 bits of one draw (exact: scaling by a power of two).
double uniform_unit(Rng& rng) {
  return static_cast<double>(rng() >> 11) * 0x1.0p-53;
}

// |value| as an unsigned number; exact for INT64_MIN.
std::uint64_t magnitude(std::int64_t value) {
  const auto bits = static_cast<std::uint64_t>(value);
  return value < 0 ? std::uint64_t{0} - bits : bits;
}

void validate(const GeneratorConfig& config) {
  if (config.device_count < 1) {
    throw std::invalid_argument("GeneratorConfig: device_count must be >= 1");
  }
  if (config.device_distribution == DeviceDistribution::Zipf &&
      !(std::isfinite(config.zipf_exponent) && config.zipf_exponent > 0.0)) {
    throw std::invalid_argument("GeneratorConfig: zipf_exponent must be finite and > 0");
  }
  if (config.max_timestamp_step < 0) {
    throw std::invalid_argument("GeneratorConfig: max_timestamp_step must be >= 0");
  }
  if (config.amount_min > config.amount_max) {
    throw std::invalid_argument("GeneratorConfig: amount_min must be <= amount_max");
  }
  if (config.row_count == 0) {
    return;  // no timestamps and no amounts: the bounds below hold trivially
  }

  // The last timestamp is at most timestamp_start + (row_count - 1) * max_timestamp_step.
  // headroom = INT64_MAX - timestamp_start, computed modulo 2^64; exact because the true value
  // lies in [0, 2^64 - 1] for every timestamp_start.
  const std::uint64_t steps = config.row_count - 1;
  const std::uint64_t headroom =
      static_cast<std::uint64_t>(kInt64Max) - static_cast<std::uint64_t>(config.timestamp_start);
  if (config.max_timestamp_step > 0 &&
      steps > headroom / static_cast<std::uint64_t>(config.max_timestamp_step)) {
    throw std::invalid_argument("GeneratorConfig: the last timestamp could exceed INT64_MAX");
  }

  // Sufficient bound for the amount-total invariant: row_count * max|amount| <= INT64_MAX.
  const std::uint64_t max_abs = std::max(magnitude(config.amount_min), magnitude(config.amount_max));
  if (max_abs > static_cast<std::uint64_t>(kInt64Max) / config.row_count) {
    throw std::invalid_argument(
        "GeneratorConfig: row_count * max(|amount_min|, |amount_max|) exceeds INT64_MAX, so "
        "SUM(amount) could overflow int64_t");
  }
}

// Draws device ids according to the configured distribution.
class DeviceSampler {
 public:
  explicit DeviceSampler(const GeneratorConfig& config)
      : device_count_(config.device_count),
        zipf_(config.device_distribution == DeviceDistribution::Zipf) {
    if (!zipf_) {
      return;
    }
    // cdf_[k] = (w_0 + ... + w_k) / (w_0 + ... + w_{n-1}) with w_k = 1 / (k + 1)^s.
    cdf_.resize(static_cast<std::size_t>(device_count_));
    double total = 0.0;
    for (std::size_t k = 0; k < cdf_.size(); ++k) {
      total += std::pow(static_cast<double>(k + 1), -config.zipf_exponent);
      cdf_[k] = total;
    }
    for (double& value : cdf_) {
      value /= total;  // the last entry is total / total == 1.0 exactly
    }
  }

  std::int32_t sample(Rng& rng) const {
    if (!zipf_) {
      return static_cast<std::int32_t>(
          uniform_below(rng, static_cast<std::uint64_t>(device_count_)));
    }
    // First k with u < cdf_[k]. u < 1.0 == cdf_.back(), so k is always in range; the clamp only
    // guards against a malformed table.
    const double u = uniform_unit(rng);
    const auto it = std::upper_bound(cdf_.begin(), cdf_.end(), u);
    const std::ptrdiff_t last = std::ssize(cdf_) - 1;
    return static_cast<std::int32_t>(std::min(std::distance(cdf_.begin(), it), last));
  }

 private:
  std::int32_t device_count_;
  bool zipf_;
  std::vector<double> cdf_;
};

// Fisher-Yates shuffle with the generator's own sampling (std::shuffle is implementation-defined).
void shuffle_rows(std::vector<Event>& rows, Rng& rng) {
  for (std::size_t i = rows.size(); i > 1; --i) {
    const auto j = static_cast<std::size_t>(uniform_below(rng, i));
    std::swap(rows[i - 1], rows[j]);
  }
}

}  // namespace

std::string_view to_string(Ordering ordering) {
  switch (ordering) {
    case Ordering::TimeSorted:
      return "time_sorted";
    case Ordering::Shuffled:
      return "shuffled";
    case Ordering::DeviceSorted:
      return "device_sorted";
  }
  return "unknown";
}

std::string_view to_string(DeviceDistribution distribution) {
  switch (distribution) {
    case DeviceDistribution::Uniform:
      return "uniform";
    case DeviceDistribution::Zipf:
      return "zipf";
  }
  return "unknown";
}

std::vector<Event> generate_events(const GeneratorConfig& config, Ordering ordering) {
  validate(config);

  Rng rng(config.seed);
  const DeviceSampler devices(config);

  // Draw order per row: timestamp step (not for the first row), device_id, amount.
  std::vector<Event> rows;
  rows.reserve(config.row_count);
  std::int64_t timestamp = config.timestamp_start;
  for (std::size_t i = 0; i < config.row_count; ++i) {
    if (i > 0) {
      timestamp += uniform_inclusive(rng, 0, config.max_timestamp_step);
    }
    const std::int32_t device_id = devices.sample(rng);
    const std::int64_t amount = uniform_inclusive(rng, config.amount_min, config.amount_max);
    rows.push_back(Event{timestamp, device_id, amount});
  }

  switch (ordering) {
    case Ordering::TimeSorted:
      break;
    case Ordering::Shuffled:
      shuffle_rows(rows, rng);
      break;
    case Ordering::DeviceSorted:
      // Stable on time order, so ties on device_id stay sorted by timestamp.
      std::stable_sort(rows.begin(), rows.end(), [](const Event& a, const Event& b) {
        return a.device_id < b.device_id;
      });
      break;
  }
  return rows;
}

}  // namespace cqe
