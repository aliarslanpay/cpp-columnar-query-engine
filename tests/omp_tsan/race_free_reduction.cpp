// ThreadSanitizer + OpenMP control (race-free): an OpenMP `reduction(+ : sum)` clause. The
// runtime combines the private copies with its own synchronization, which an OpenMP-unaware
// tool cannot see.
//
// Exit codes: 0 = correct sum with at least two threads, 1 = wrong sum,
// 3 = fewer than two threads were used.

#include <omp.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kThreads = 4;
constexpr std::size_t kValues = std::size_t{1} << 16;

}  // namespace

int main() {
  std::vector<std::int64_t> values(kValues);
  std::int64_t expected = 0;
  for (std::size_t i = 0; i < kValues; ++i) {
    values[i] = static_cast<std::int64_t>(i % 997) - 400;
    expected += values[i];
  }

  std::int64_t sum = 0;
  int threads = 0;
#pragma omp parallel num_threads(kThreads) reduction(+ : sum)
  {
#pragma omp single
    threads = omp_get_num_threads();
#pragma omp for schedule(static)
    for (std::size_t i = 0; i < kValues; ++i) {
      sum += values[i];
    }
  }

  if (sum != expected) {
    std::printf("race_free_reduction: FAIL sum %lld expected %lld\n", static_cast<long long>(sum),
                static_cast<long long>(expected));
    return 1;
  }
  std::printf("threads=%d\n", threads);
  if (threads < 2) {
    std::printf("race_free_reduction: FAIL fewer than two threads\n");
    return 3;
  }
  std::printf("race_free_reduction: OK\n");
  return 0;
}
