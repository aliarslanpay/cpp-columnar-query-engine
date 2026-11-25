// ThreadSanitizer + OpenMP control (race-free): each thread writes its own plain (non-atomic)
// cell, then reads a neighbour's cell after `#pragma omp barrier`. The barrier is the only
// ordering between the write and the read, so a tool that does not understand OpenMP barriers
// would report a false race here.
//
// Exit codes: 0 = correct values with at least two threads, 1 = wrong values,
// 3 = fewer than two threads were used.

#include <omp.h>

#include <cstddef>
#include <cstdio>
#include <vector>

namespace {

constexpr int kThreads = 4;

}  // namespace

int main() {
  std::vector<int> data(kThreads, 0);
  std::vector<int> seen(kThreads, -1);
  int threads = 0;
#pragma omp parallel num_threads(kThreads)
  {
    const int t = omp_get_thread_num();
    const int n = omp_get_num_threads();
    data[static_cast<std::size_t>(t)] = t + 1;
#pragma omp barrier
    seen[static_cast<std::size_t>(t)] = data[static_cast<std::size_t>((t + 1) % n)];
    if (t == 0) {
      threads = n;
    }
  }

  for (int t = 0; t < threads; ++t) {
    const int expected = (t + 1) % threads + 1;
    if (seen[static_cast<std::size_t>(t)] != expected) {
      std::printf("race_free_barrier: FAIL thread %d saw %d expected %d\n", t,
                  seen[static_cast<std::size_t>(t)], expected);
      return 1;
    }
  }
  std::printf("threads=%d\n", threads);
  if (threads < 2) {
    std::printf("race_free_barrier: FAIL fewer than two threads\n");
    return 3;
  }
  std::printf("race_free_barrier: OK\n");
  return 0;
}
