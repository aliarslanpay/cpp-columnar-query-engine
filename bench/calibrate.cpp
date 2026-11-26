#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "cqe/executor.hpp"
#include "cqe/reference.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace cqe;

constexpr std::size_t kCapacity = BlockLayout::kDefaultCapacity;
constexpr std::size_t kBlocks = 128;
constexpr int kSamples = 5;
constexpr auto kMinimumSample = std::chrono::nanoseconds{std::chrono::milliseconds{5}};
constexpr std::array<int, 4> kThreadBudgets{1, 2, 4, 8};

std::vector<Event> make_rows() {
  std::vector<Event> rows;
  rows.reserve(kBlocks * kCapacity);
  for (std::size_t i = 0; i < kBlocks * kCapacity; ++i) {
    std::int32_t device = i % 2 == 0 ? 0 : 9;
    if (i == kCapacity + 1) {
      device = 4;
    }
    rows.push_back({static_cast<std::int64_t>(i), device,
                    static_cast<std::int64_t>(i % 17) - 8});
  }
  return rows;
}

struct Batch {
  std::int64_t elapsed_ns;
  std::uint64_t checksum;
};

template <class Operation>
Batch timed_batch(const ColumnStore& store, std::size_t repetitions, const Operation& operation) {
  const auto* timestamps = store.timestamps().data();
  const auto* devices = store.device_ids().data();
  const auto* amounts = store.amounts().data();
  std::uint64_t checksum = 0;
  const auto start = Clock::now();
  for (std::size_t i = 0; i < repetitions; ++i) {
    __asm__ __volatile__("" : : "g"(timestamps), "g"(devices), "g"(amounts) : "memory");
    const std::uint64_t value = operation();
    __asm__ __volatile__("" : : "g"(value));
    checksum += value;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
  return {elapsed.count(), checksum};
}

template <class Operation>
void measure(const ColumnStore& store, std::string_view scenario, std::string_view operation_name,
             std::size_t candidates, int budget, const Operation& operation) {
  (void)timed_batch(store, 1, operation);  // Warm up, including the OpenMP team.
  std::size_t repetitions = 1;
  while (timed_batch(store, repetitions, operation).elapsed_ns < kMinimumSample.count()) {
    repetitions *= 2;
  }
  for (int sample = 1; sample <= kSamples; ++sample) {
    const Batch batch = timed_batch(store, repetitions, operation);
    std::cout << scenario << ',' << operation_name << ',' << candidates << ',' << budget << ','
              << sample << ',' << repetitions << ',' << batch.elapsed_ns << ','
              << static_cast<double>(batch.elapsed_ns) / static_cast<double>(repetitions) << ','
              << batch.checksum << '\n';
  }
}

std::uint64_t checksum_for(const Execution& execution) {
  return static_cast<std::uint64_t>(execution.result.count) ^
         static_cast<std::uint64_t>(execution.result.sum) ^ execution.stats.rows_examined;
}

template <class Operation>
bool measure_scan(const ColumnStore& store, std::string_view scenario, std::string_view name,
                  int budget, const QueryResult& expected, const Operation& execute) {
  const Execution execution = execute();
  if (execution.result != expected) {
    std::cerr << "Result mismatch: " << scenario << '/' << name << ", threads=" << budget << '\n';
    return false;
  }
  measure(store, scenario, name, execution.stats.blocks_scanned, budget,
          [&] { return checksum_for(execute()); });
  return true;
}

bool run_case(const IndexedTable& indexed, std::span<const Event> rows, const Query& query,
              std::string_view scenario, bool index_verified) {
  const auto& table = indexed.table();
  const auto& store = table.store();
  const auto& zone_map = table.zone_map();
  const auto& index = indexed.device_index();
  const auto* equals = std::get_if<DeviceEquals>(&query.predicate);
  const QueryResult expected = reference_evaluate(rows, query);
  measure(store, scenario, "zone_candidates", zone_map.candidate_blocks(query.predicate).size(), 1,
          [&] { return zone_map.candidate_blocks(query.predicate).size(); });
  if (equals != nullptr && index_verified) {
    measure(store, scenario, "index_candidates", index.candidate_blocks(equals->value).size(), 1,
            [&] { return index.candidate_blocks(equals->value).size(); });
  }

  bool verified = true;
  for (const int budget : kThreadBudgets) {
    const ScanOptions options{ScanMode::Fused, budget};
    verified &= measure_scan(store, scenario, "full", budget, expected,
                             [&] { return full_scan(table, query, options); });
    verified &= measure_scan(store, scenario, "zone_map", budget, expected,
                             [&] { return zone_map_scan(table, query, options); });
    if (equals != nullptr && index_verified) {
      verified &= measure_scan(store, scenario, "index", budget, expected,
                               [&] { return index_scan(indexed, *equals, options); });
    }
    verified &= measure_scan(store, scenario, "auto", budget, expected,
                             [&] { return auto_scan(indexed, query, budget).execution; });
  }
  return verified;
}

}  // namespace

int main() {
  using namespace cqe;
  const auto rows = make_rows();
  ZoneMappedTable table{rows, kCapacity};
  const auto start = Clock::now();
  const IndexedTable indexed{std::move(table)};
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
  bool index_verified = true;
  for (const std::int32_t device : {0, 4, 6}) {
    const DeviceEquals predicate{device};
    if (index_scan(indexed, predicate).result != reference_evaluate(rows, Query{predicate})) {
      std::cerr << "Index result mismatch for device " << device << '\n';
      index_verified = false;
    }
  }
  std::cerr << "rows=" << rows.size() << " block_capacity=" << kCapacity
            << " index_build_ns=" << elapsed.count()
            << " index_logical_bytes=" << indexed.device_index().logical_bytes()
            << " index_verified=" << (index_verified ? "yes" : "no") << '\n';

  std::cout << "scenario,operation,candidate_blocks,thread_budget,sample,repetitions,elapsed_ns,"
               "ns_per_call,checksum\n";
  bool verified = index_verified;
  for (const auto& [scenario, blocks] :
       {std::pair{std::string_view{"range_0"}, std::size_t{0}},
        {std::string_view{"range_1"}, std::size_t{1}},
        {std::string_view{"range_4"}, std::size_t{4}},
        {std::string_view{"range_16"}, std::size_t{16}},
        {std::string_view{"range_64"}, std::size_t{64}},
        {std::string_view{"range_128"}, kBlocks}}) {
    const Query query{TimestampRange{0, static_cast<std::int64_t>(blocks * kCapacity) - 1}};
    verified &= run_case(indexed, rows, query, scenario, index_verified);
  }
  for (const auto& [scenario, device] :
       {std::pair{std::string_view{"device_common"}, std::int32_t{0}},
        {std::string_view{"device_rare"}, std::int32_t{4}},
        {std::string_view{"device_absent"}, std::int32_t{6}}}) {
    verified &= run_case(indexed, rows, Query{DeviceEquals{device}}, scenario, index_verified);
  }
  return verified ? 0 : 1;
}
