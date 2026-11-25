// Contract tests for scan_blocks_parallel (parallel_scan.hpp), using fake block scanners.

#include "cqe/parallel_scan.hpp"

#include <gtest/gtest.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "test_util.hpp"

namespace cqe {

inline void PrintTo(const BlockPartial& p, std::ostream* os) {
  *os << "{blocks=" << p.blocks << ", rows=" << p.rows_examined << ", count=" << p.count
      << ", sum=" << p.sum << "}";
}

namespace {

using test::kInt64Max;
using test::kInt64Min;

int current_thread() {
#ifdef _OPENMP
  return omp_get_thread_num();
#else
  return 0;
#endif
}

// Returns a deterministic partial per block and records what each call saw. Every record lives
// in a per-block slot, so concurrent calls for different blocks never write the same memory.
class FakeScanner final : public BlockScanner {
 public:
  FakeScanner(std::size_t blocks, std::size_t scratch_size)
      : blocks_(blocks),
        scratch_size_(scratch_size),
        calls_(blocks),
        scratch_ok_(blocks, 0),
        scratch_data_(blocks, nullptr),
        thread_(blocks, -1) {}

  static BlockPartial partial_for(std::size_t i) {
    return BlockPartial{1, i % 5 + 1, static_cast<std::int64_t>(i % 3),
                        static_cast<std::int64_t>(i % 7) - 3};
  }

  static BlockPartial expected_total(std::size_t blocks) {
    BlockPartial total;
    for (std::size_t i = 0; i < blocks; ++i) {
      const BlockPartial p = partial_for(i);
      total.blocks += p.blocks;
      total.rows_examined += p.rows_examined;
      total.count += p.count;
      total.sum += p.sum;
    }
    return total;
  }

  [[nodiscard]] std::size_t block_count() const noexcept override { return blocks_; }
  [[nodiscard]] std::size_t scratch_size() const noexcept override { return scratch_size_; }

  [[nodiscard]] BlockPartial scan_block(std::size_t i,
                                        std::span<RowOffset> scratch) const noexcept override {
    calls_[i].fetch_add(1, std::memory_order_relaxed);
    // Fill the scratch with this block's tag and read it back: a buffer that another concurrent
    // call also used would be overwritten (and ThreadSanitizer would report the race).
    bool ok = scratch.size() == scratch_size_;
    const auto tag = static_cast<RowOffset>(i);
    for (RowOffset& x : scratch) {
      x = tag;
    }
    for (const RowOffset x : scratch) {
      ok = ok && x == tag;
    }
    scratch_ok_[i] = ok ? 1 : 0;
    scratch_data_[i] = scratch.empty() ? nullptr : scratch.data();
    thread_[i] = current_thread();
    return partial_for(i);
  }

  void expect_each_block_scanned_once() const {
    for (std::size_t i = 0; i < blocks_; ++i) {
      EXPECT_EQ(calls_[i].load(), 1) << "block " << i;
    }
  }

  void expect_scratch_ok() const {
    for (std::size_t i = 0; i < blocks_; ++i) {
      EXPECT_EQ(scratch_ok_[i], 1) << "block " << i << ": wrong size or overwritten scratch";
    }
  }

  // Distinct non-null scratch buffers, each mapped to the threads that used it.
  [[nodiscard]] std::map<const RowOffset*, std::set<int>> scratch_users() const {
    std::map<const RowOffset*, std::set<int>> users;
    for (std::size_t i = 0; i < blocks_; ++i) {
      if (scratch_data_[i] != nullptr) {
        users[scratch_data_[i]].insert(thread_[i]);
      }
    }
    return users;
  }

  [[nodiscard]] bool all_scratch_empty() const {
    for (const RowOffset* p : scratch_data_) {
      if (p != nullptr) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::set<int> threads_seen() const { return {thread_.begin(), thread_.end()}; }

 private:
  std::size_t blocks_;
  std::size_t scratch_size_;
  mutable std::vector<std::atomic<int>> calls_;
  mutable std::vector<int> scratch_ok_;  // not vector<bool>: elements are written concurrently
  mutable std::vector<const RowOffset*> scratch_data_;
  mutable std::vector<int> thread_;
};

// Returns given partials, for values near the int64 limits.
class FixedScanner final : public BlockScanner {
 public:
  explicit FixedScanner(std::vector<BlockPartial> partials) : partials_(std::move(partials)) {}
  [[nodiscard]] std::size_t block_count() const noexcept override { return partials_.size(); }
  [[nodiscard]] std::size_t scratch_size() const noexcept override { return 0; }
  [[nodiscard]] BlockPartial scan_block(std::size_t i,
                                        std::span<RowOffset>) const noexcept override {
    return partials_[i];
  }

 private:
  std::vector<BlockPartial> partials_;
};

constexpr int kThreadCounts[] = {1, 2, 3, 4, 8, 16};

TEST(ScanBlocksParallel, RejectsInvalidThreadCounts) {
  const FakeScanner some(10, 0);
  const FakeScanner none(0, 0);
  for (const int threads : {0, -1, std::numeric_limits<int>::min()}) {
    EXPECT_THROW((void)scan_blocks_parallel(some, threads), std::invalid_argument) << threads;
    EXPECT_THROW((void)scan_blocks_parallel(none, threads), std::invalid_argument) << threads;
  }
}

TEST(ScanBlocksParallel, NoBlocksGivesAnEmptyPartial) {
  for (const int threads : kThreadCounts) {
    const FakeScanner none(0, 16);
    EXPECT_EQ(scan_blocks_parallel(none, threads), BlockPartial{}) << "threads " << threads;
  }
}

TEST(ScanBlocksParallel, SumsEveryBlockExactlyOnce) {
  constexpr std::size_t kBlockCounts[] = {1, 2, 7, 64, 1000};
  for (const std::size_t blocks : kBlockCounts) {
    for (const int threads : kThreadCounts) {
      const FakeScanner scanner(blocks, 8);
      EXPECT_EQ(scan_blocks_parallel(scanner, threads), FakeScanner::expected_total(blocks))
          << "blocks " << blocks << " threads " << threads;
      scanner.expect_each_block_scanned_once();
      scanner.expect_scratch_ok();
    }
  }
}

TEST(ScanBlocksParallel, NoScratchWhenNoneIsNeeded) {
  for (const int threads : kThreadCounts) {
    const FakeScanner scanner(50, 0);
    EXPECT_EQ(scan_blocks_parallel(scanner, threads), FakeScanner::expected_total(50));
    scanner.expect_scratch_ok();
    EXPECT_TRUE(scanner.all_scratch_empty()) << "threads " << threads;
  }
}

TEST(ScanBlocksParallel, EachWorkerHasItsOwnScratch) {
  const FakeScanner scanner(256, 64);
  EXPECT_EQ(scan_blocks_parallel(scanner, 4), FakeScanner::expected_total(256));
  scanner.expect_scratch_ok();
  const auto users = scanner.scratch_users();
  EXPECT_GE(users.size(), 1U);
  EXPECT_LE(users.size(), 4U) << "at most min(threads, blocks) scratch buffers";
  for (const auto& [buffer, threads] : users) {
    EXPECT_EQ(threads.size(), 1U) << "a scratch buffer was used by more than one thread";
  }
}

TEST(ScanBlocksParallel, ScratchBuffersAreLimitedByTheNumberOfBlocks) {
  const FakeScanner scanner(2, 32);
  EXPECT_EQ(scan_blocks_parallel(scanner, 8), FakeScanner::expected_total(2));
  scanner.expect_scratch_ok();
  EXPECT_LE(scanner.scratch_users().size(), 2U) << "at most min(threads, blocks) scratch buffers";
}

TEST(ScanBlocksParallel, ExactAtTheInt64LimitsOfTheAmountTotalInvariant) {
  // Eight positive partial sums that add up to exactly INT64_MAX, and eight negative ones that add
  // up to exactly INT64_MIN (INT64_MIN / 8 = -2^60). Every grouping of either set stays in range.
  std::vector<BlockPartial> positive(8, BlockPartial{1, 1, 1, kInt64Max / 8});
  positive.back().sum += kInt64Max % 8;
  const std::vector<BlockPartial> negative(8, BlockPartial{1, 1, 1, kInt64Min / 8});
  for (const int threads : kThreadCounts) {
    EXPECT_EQ(scan_blocks_parallel(FixedScanner(positive), threads),
              (BlockPartial{8, 8, 8, kInt64Max}))
        << "threads " << threads;
    EXPECT_EQ(scan_blocks_parallel(FixedScanner(negative), threads),
              (BlockPartial{8, 8, 8, kInt64Min}))
        << "threads " << threads;
  }
}

TEST(ScanBlocksParallel, UsesAtLeastTwoThreadsWhenAskedForFour) {
#ifndef _OPENMP
  GTEST_SKIP() << "built without OpenMP";
#else
  const FakeScanner scanner(64, 0);
  EXPECT_EQ(scan_blocks_parallel(scanner, 4), FakeScanner::expected_total(64));
  EXPECT_GE(scanner.threads_seen().size(), 2U)
      << "the blocks were scanned by " << scanner.threads_seen().size() << " thread(s)";
#endif
}

TEST(ScanBlocksParallel, OneThreadUsesOneThread) {
  const FakeScanner scanner(64, 0);
  EXPECT_EQ(scan_blocks_parallel(scanner, 1), FakeScanner::expected_total(64));
  EXPECT_EQ(scanner.threads_seen().size(), 1U);
}

}  // namespace
}  // namespace cqe
