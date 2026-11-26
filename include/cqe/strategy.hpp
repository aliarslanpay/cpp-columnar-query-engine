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

// Choose a scan and a worker limit from candidate counts alone, using costs calibrated for
// fused scans. The candidate lists have already been obtained and are reused by the executor;
// the decision compares the remaining work. Account for parallel setup as well as scan work.
// Candidate counts are at most total_blocks and max_threads is at least 1. Only an available
// strategy may be chosen. A zero-candidate path runs serially and wins over scanning any blocks.
// The chosen worker limit is in [1, max_threads] and never exceeds its candidate count when
// that count is nonzero. Parallel plans must be considered for large scans if max_threads > 1.
[[nodiscard]] ExecutionPlan choose_strategy(const StrategyEstimates& estimates,
                                            int max_threads) noexcept;

}  // namespace cqe
