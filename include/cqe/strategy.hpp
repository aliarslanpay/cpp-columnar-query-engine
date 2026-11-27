#pragma once

#include <cstddef>
#include <optional>

namespace cqe {

enum class ScanStrategy {
  Full,
  ZoneMap,
  Index,
};

struct StrategyEstimates {
  std::size_t total_blocks = 0;
  std::size_t zone_map_blocks = 0;
  // A missing count means index scans are unavailable for this predicate.
  std::optional<std::size_t> index_blocks;
};

struct ExecutionPlan {
  ScanStrategy strategy = ScanStrategy::Full;
  int threads = 1;
};

// Chooses a scan and a worker limit from candidate counts alone, using costs calibrated for
// fused scans. The candidate lists have already been obtained and are reused by the executor,
// so the decision compares only the remaining work, including the setup cost of a parallel
// scan. Teams of 2, 4 and 8 workers are considered when max_threads allows them.
// Preconditions: candidate counts are at most total_blocks, and max_threads is at least 1.
// Guarantees: only an available strategy is chosen; a strategy with zero candidates is chosen,
// serially, over any plan that scans blocks; the worker limit is in [1, max_threads] and never
// exceeds the chosen strategy's candidate count when that count is nonzero.
[[nodiscard]] ExecutionPlan choose_strategy(const StrategyEstimates& estimates,
                                            int max_threads) noexcept;

}  // namespace cqe
