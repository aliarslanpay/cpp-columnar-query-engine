#include "cqe/strategy.hpp"

#include <array>

namespace cqe {
namespace {

// Nanoseconds for fused scans with blocks of 1,024 rows. Reproduce the estimates with
// python bench/fit_costs.py results/scan-calibration.csv.
constexpr double kSerialFullNsPerBlock = 658.25;

struct BlockScanCost {
  int workers;
  double setup_ns;
  double ns_per_block;
};

constexpr std::array<BlockScanCost, 4> kBlockCosts{{
    {1, 0.00, 819.56}, {2, 1278.88, 350.88},
    {4, 1085.69, 260.72}, {8, 2566.59, 182.35},
}};

}  // namespace

ExecutionPlan choose_strategy(const StrategyEstimates& estimates, int max_threads) noexcept {
  ExecutionPlan best{ScanStrategy::Full, 1};
  double best_ns = kSerialFullNsPerBlock * static_cast<double>(estimates.total_blocks);
  if (estimates.total_blocks == 0) {
    return best;
  }

  if (estimates.zone_map_blocks == 0) {
    return {ScanStrategy::ZoneMap, 1};
  }
  if (estimates.index_blocks == 0) {
    return {ScanStrategy::Index, 1};
  }

  // The serial full path was priced above. All other scans use the block executor.
  const auto consider = [&](ScanStrategy strategy, std::size_t blocks) {
    for (const auto& cost : kBlockCosts) {
      if (cost.workers > max_threads || static_cast<std::size_t>(cost.workers) > blocks ||
          (strategy == ScanStrategy::Full && cost.workers == 1)) {
        continue;
      }
      const double ns = cost.setup_ns + cost.ns_per_block * static_cast<double>(blocks);
      if (ns < best_ns) {
        best_ns = ns;
        best = {strategy, cost.workers};
      }
    }
  };
  consider(ScanStrategy::Full, estimates.total_blocks);
  consider(ScanStrategy::ZoneMap, estimates.zone_map_blocks);
  if (estimates.index_blocks) {
    consider(ScanStrategy::Index, *estimates.index_blocks);
  }
  return best;
}

}  // namespace cqe
