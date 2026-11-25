#include "cqe/parallel_scan.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace cqe {
namespace {

void add_partial(BlockPartial& total, const BlockPartial& part) noexcept {
  total.blocks += part.blocks;
  total.rows_examined += part.rows_examined;
  total.count += part.count;
  total.sum += part.sum;
}

#ifdef _OPENMP
// Give each worker's partial its own cache line on the target x86-64 system.
struct alignas(64) WorkerState {
  BlockPartial partial;
  std::vector<RowOffset> scratch;
};
#endif

}  // namespace

BlockPartial scan_blocks_parallel(const BlockScanner& scanner, int threads) {
  if (threads < 1) {
    throw std::invalid_argument("scan_blocks_parallel: threads must be >= 1");
  }
  const std::size_t blocks = scanner.block_count();
  if (blocks == 0) {
    return BlockPartial{};
  }
  const std::size_t scratch_size = scanner.scratch_size();

#ifdef _OPENMP
  const int worker_count = static_cast<int>(std::min(blocks, static_cast<std::size_t>(threads)));
  if (worker_count > 1) {
    std::vector<WorkerState> workers(static_cast<std::size_t>(worker_count));
    if (scratch_size != 0) {
      for (WorkerState& worker : workers) {
        worker.scratch.resize(scratch_size);
      }
    }

#pragma omp parallel num_threads(worker_count) default(none) shared(scanner, blocks, workers)
    {
      WorkerState& worker = workers[static_cast<std::size_t>(omp_get_thread_num())];
      BlockPartial partial;
#pragma omp for schedule(static) nowait
      for (std::size_t i = 0; i < blocks; ++i) {
        add_partial(partial, scanner.scan_block(i, worker.scratch));
      }
      worker.partial = partial;
    }

    // The parallel region's join makes all partials visible. If the runtime used a smaller
    // team, unused slots still contain zeroes.
    BlockPartial total;
    for (const WorkerState& worker : workers) {
      add_partial(total, worker.partial);
    }
    return total;
  }
#endif

  std::vector<RowOffset> scratch(scratch_size);
  BlockPartial total;
  for (std::size_t i = 0; i < blocks; ++i) {
    add_partial(total, scanner.scan_block(i, scratch));
  }
  return total;
}

}  // namespace cqe
