#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "cqe/kernels.hpp"

namespace cqe {

// The result of scanning one block, or the field-wise sum of several block results.
struct BlockPartial {
  std::uint64_t blocks = 0;         // blocks scanned
  std::uint64_t rows_examined = 0;  // rows of the scanned blocks
  std::int64_t count = 0;           // matching rows
  std::int64_t sum = 0;             // SUM(amount) over the matching rows

  friend bool operator==(const BlockPartial&, const BlockPartial&) = default;
};

// The per-block work of a scan, independent of how blocks are distributed over threads.
// The executor implements it for each mode and predicate; tests use fakes.
class BlockScanner {
 public:
  virtual ~BlockScanner() = default;

  // Number of blocks to scan; they are numbered 0 .. block_count() - 1.
  [[nodiscard]] virtual std::size_t block_count() const noexcept = 0;

  // Scratch elements one worker needs (the rows of the largest block); 0 if no scratch is
  // needed, as in fused mode.
  [[nodiscard]] virtual std::size_t scratch_size() const noexcept = 0;

  // Scans block i (< block_count()) using `scratch`, which has exactly scratch_size() elements
  // and is not used by any other concurrent call. Safe to call concurrently for different i.
  [[nodiscard]] virtual BlockPartial scan_block(std::size_t i,
                                                std::span<RowOffset> scratch) const noexcept = 0;

 protected:
  BlockScanner() = default;
  BlockScanner(const BlockScanner&) = default;
  BlockScanner& operator=(const BlockScanner&) = default;
};

// Parallel scan of all blocks with per-thread partial results.
//
// Returns the field-wise sum of scanner.scan_block(i, ...) over every i in
// [0, scanner.block_count()). Each block is scanned exactly once.
//
// - threads >= 1, checked before anything else; otherwise std::invalid_argument, also when there
//   are no blocks.
// - block_count() == 0: returns BlockPartial{} without opening a parallel region and without
//   allocating anything.
// - Workers: at most min(threads, block_count()) OpenMP threads.
// - Scratch: when scratch_size() > 0, one buffer of scratch_size() elements is allocated per
//   worker, at most min(threads, block_count()) buffers, before any parallel region. A buffer is
//   used by one worker only. When scratch_size() == 0, nothing is allocated and every call
//   receives an empty span. A failed allocation (std::bad_alloc) happens before any region.
// - Inside the parallel region: no allocation and no exception. Each thread accumulates into its
//   own partial, with no atomics, no critical sections and no writes to shared state. Partials
//   are combined once, after the parallel loop. Keeping the per-thread partials on separate cache
//   lines is a performance requirement, not a correctness one.
// - The result does not depend on the number of threads or on scheduling. Integer addition is
//   associative and commutative, and the amount-total invariant keeps every partial sum and
//   every combination of them in range.
// - Built without OpenMP, the function runs serially with the same result.
[[nodiscard]] BlockPartial scan_blocks_parallel(const BlockScanner& scanner, int threads);

}  // namespace cqe
