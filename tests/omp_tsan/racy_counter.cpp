// ThreadSanitizer + OpenMP control (real race): threads increment one shared, non-atomic counter
// without synchronization. ThreadSanitizer must report a data race; with halt_on_error=1 it stops
// the program with its exit code (66 by default).
//
// The thread count is printed before the racing loop, because the first report ends the program.
// Exit codes without a report: 0 = no race was reported, 3 = fewer than two threads were used.

#include <omp.h>

#include <cstdio>

namespace {

constexpr int kThreads = 4;
constexpr int kIterations = 100000;

// volatile keeps every increment as its own load and store. Without it, the compiler turns each
// thread's loop into one load and one store, and ThreadSanitizer then missed the race in some
// runs. volatile does not synchronize anything, so the increments still race.
volatile long counter = 0;

}  // namespace

int main() {
  int threads = 0;
#pragma omp parallel num_threads(kThreads)
  {
#pragma omp single
    {
      threads = omp_get_num_threads();
      std::printf("threads=%d\n", threads);
      std::fflush(stdout);
    }
#pragma omp for schedule(static)
    for (int i = 0; i < kIterations; ++i) {
      counter = counter + 1;  // the data race under test (C++20 deprecates ++ on volatile)
    }
  }

  std::printf("racy_counter: no race reported, counter=%ld\n", counter);
  return threads < 2 ? 3 : 0;
}
