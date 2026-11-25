// ThreadSanitizer + OpenMP control (race-free): per-thread partial sums in separate, padded slots,
// combined after each parallel region. Several regions run back to back because the OpenMP
// runtime reuses its worker threads between regions.
//
// Exit codes: 0 = correct result with at least two threads, 1 = wrong result,
// 3 = fewer than two threads were used (the check would be vacuous).

#include <omp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kThreads = 4;
constexpr std::size_t kValues = std::size_t{1} << 16;
constexpr int kRegions = 8;

struct alignas(64) Slot {
  std::int64_t value = 0;
};

}  // namespace

int main() {
  std::vector<std::int64_t> values(kValues);
  std::int64_t expected = 0;
  for (std::size_t i = 0; i < kValues; ++i) {
    values[i] = static_cast<std::int64_t>(i % 1000) - 500;
    expected += values[i];
  }

  int fewest_threads = kThreads;
  for (int region = 0; region < kRegions; ++region) {
    std::vector<Slot> slots(kThreads);
    int threads_in_region = 0;
#pragma omp parallel num_threads(kThreads)
    {
      const auto tid = static_cast<std::size_t>(omp_get_thread_num());
#pragma omp single
      threads_in_region = omp_get_num_threads();
#pragma omp for schedule(static)
      for (std::size_t i = 0; i < kValues; ++i) {
        slots[tid].value += values[i];
      }
    }
    std::int64_t total = 0;
    for (const Slot& slot : slots) {
      total += slot.value;
    }
    if (total != expected) {
      std::printf("race_free_partials: FAIL region %d total %lld expected %lld\n", region,
                  static_cast<long long>(total), static_cast<long long>(expected));
      return 1;
    }
    fewest_threads = std::min(fewest_threads, threads_in_region);
  }

  std::printf("threads=%d\n", fewest_threads);
  if (fewest_threads < 2) {
    std::printf("race_free_partials: FAIL fewer than two threads\n");
    return 3;
  }
  std::printf("race_free_partials: OK\n");
  return 0;
}
