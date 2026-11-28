// Zone-map pruning on the same rows in two orderings: time-sorted (timestamps clustered by
// block) and shuffled with a fixed seed. One table size, one block capacity, one worker, and
// one narrow timestamp range. Uses the timing method of calibrate.cpp.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "cqe/dataset.hpp"
#include "cqe/executor.hpp"
#include "cqe/reference.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace cqe;

constexpr std::size_t kRows = std::size_t{1} << 20;
constexpr std::size_t kCapacity = BlockLayout::kDefaultCapacity;
constexpr std::uint64_t kSeed = 7;
constexpr int kSamples = 5;
constexpr auto kMinimumSample = std::chrono::nanoseconds{std::chrono::milliseconds{5}};
constexpr ScanOptions kSerial{ScanMode::Fused, 1};

struct Batch {
  std::int64_t elapsed_ns;
  std::uint64_t checksum;
};

std::uint64_t checksum_for(const Execution& execution) {
  return static_cast<std::uint64_t>(execution.result.count) ^
         static_cast<std::uint64_t>(execution.result.sum) ^ execution.stats.rows_examined;
}

template <class Operation>
Batch timed_batch(const ColumnStore& store, std::size_t repetitions, const Operation& operation) {
  const auto* timestamps = store.timestamps().data();
  const auto* devices = store.device_ids().data();
  const auto* amounts = store.amounts().data();
  std::uint64_t checksum = 0;
  const auto start = Clock::now();
  for (std::size_t i = 0; i < repetitions; ++i) {
    __asm__ __volatile__("" : : "g"(timestamps), "g"(devices), "g"(amounts) : "memory");
    const std::uint64_t value = checksum_for(operation());
    __asm__ __volatile__("" : : "g"(value));
    checksum += value;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
  return {elapsed.count(), checksum};
}

template <class Operation>
void measure(const ColumnStore& store, std::string_view ordering, std::string_view name,
             const Execution& execution, const Operation& operation) {
  (void)timed_batch(store, 1, operation);  // Warm up.
  std::size_t repetitions = 1;
  while (timed_batch(store, repetitions, operation).elapsed_ns < kMinimumSample.count()) {
    repetitions *= 2;
  }
  const ExecutionStats& stats = execution.stats;
  for (int sample = 1; sample <= kSamples; ++sample) {
    const Batch batch = timed_batch(store, repetitions, operation);
    std::cout << ordering << ',' << name << ',' << stats.blocks_total << ','
              << stats.blocks_scanned << ',' << stats.rows_examined << ',' << stats.rows_matched
              << ',' << sample << ',' << repetitions << ',' << batch.elapsed_ns << ','
              << static_cast<double>(batch.elapsed_ns) / static_cast<double>(repetitions) << ','
              << batch.checksum << '\n';
  }
}

}  // namespace

int main() {
  GeneratorConfig config;
  config.row_count = kRows;
  config.seed = kSeed;
  const std::vector<Event> clustered = generate_events(config, Ordering::TimeSorted);
  const std::vector<Event> shuffled = generate_events(config, Ordering::Shuffled);

  // About 0.1% of the rows, taken from the middle of the time order.
  const std::size_t first = kRows / 2;
  const Query query{TimestampRange{clustered[first].timestamp,
                                   clustered[first + kRows / 1000 - 1].timestamp}};
  const auto& range = std::get<TimestampRange>(query.predicate);

  // The reference is computed independently on each row vector; equal multisets must agree.
  const QueryResult expected = reference_evaluate(clustered, query);
  bool verified = reference_evaluate(shuffled, query) == expected;
  std::cerr << "rows=" << kRows << " block_capacity=" << kCapacity << " seed=" << kSeed
            << " lo=" << range.lo << " hi=" << range.hi << " reference_count=" << expected.count
            << " reference_sum=" << expected.sum << '\n';

  std::cout << "ordering,operation,blocks_total,blocks_scanned,rows_examined,rows_matched,sample,"
               "repetitions,elapsed_ns,ns_per_call,checksum\n";
  const std::array<std::pair<std::string_view, const std::vector<Event>*>, 2> orderings{
      {{"clustered", &clustered}, {"shuffled", &shuffled}}};
  for (const auto& [name, rows] : orderings) {
    const ZoneMappedTable table{*rows, kCapacity};
    const auto full = [&] { return full_scan(table, query, kSerial); };
    const auto pruned = [&] { return zone_map_scan(table, query, kSerial); };
    const Execution full_execution = full();
    const Execution pruned_execution = pruned();
    const bool ok = full_execution.result == expected && pruned_execution.result == expected;
    std::cerr << name << ": full=" << (full_execution.result == expected ? "match" : "MISMATCH")
              << " zone_map=" << (pruned_execution.result == expected ? "match" : "MISMATCH")
              << " blocks_scanned=" << pruned_execution.stats.blocks_scanned << '/'
              << pruned_execution.stats.blocks_total << '\n';
    verified &= ok;
    if (!ok) {
      continue;
    }
    measure(table.store(), name, "full", full_execution, full);
    measure(table.store(), name, "zone_map", pruned_execution, pruned);
  }
  std::cerr << "verified=" << (verified ? "yes" : "no") << '\n';
  return verified ? 0 : 1;
}
