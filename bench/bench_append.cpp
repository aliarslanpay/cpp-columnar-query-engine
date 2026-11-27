#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "cqe/executor.hpp"
#include "cqe/reference.hpp"

namespace {
using namespace cqe;
using Clock = std::chrono::steady_clock;
constexpr std::size_t capacity = 1024;
constexpr int devices = 97;

template<class Operation>
std::int64_t time_ns(Operation operation) {
  const auto begin = Clock::now();
  operation();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
}

void verify(const IndexedTable& maintained, const IndexedTable& rebuilt, std::span<const Event> rows) {
  const auto& left = maintained.table();
  const auto& right = rebuilt.table();
  if (!std::ranges::equal(left.store().timestamps(), right.store().timestamps()) ||
      !std::ranges::equal(left.store().device_ids(), right.store().device_ids()) ||
      !std::ranges::equal(left.store().amounts(), right.store().amounts()) ||
      !std::ranges::equal(left.zone_map().zones(), right.zone_map().zones()))
    throw std::runtime_error("append data mismatch");
  for (int id = 0; id < devices; ++id) {
    if (!std::ranges::equal(maintained.device_index().candidate_blocks(id),
                            rebuilt.device_index().candidate_blocks(id)))
      throw std::runtime_error("append posting mismatch");
  }
  const DeviceEquals predicate{rows.back().device_id};
  const auto expected = reference_evaluate(rows, Query{predicate});
  if (index_scan(maintained, predicate).result != expected || index_scan(rebuilt, predicate).result != expected)
    throw std::runtime_error("append query mismatch");
}

void compare(std::string_view scenario, std::size_t batch_size, std::size_t steps) {
  std::mt19937 rng(37);
  constexpr std::size_t initial = 32 * capacity - 13;
  std::vector<Event> input;
  for (std::size_t i = 0; i < initial + steps * batch_size; ++i) {
    const int id = i < initial ? static_cast<int>(i / capacity) : static_cast<int>(rng() % devices);
    input.push_back({static_cast<std::int64_t>(i), id, static_cast<int>(rng() % 31) - 15});
  }
  const std::span<const Event> rows{input};
  for (int sample = 0; sample <= 5; ++sample) {
    IndexedTable maintained{ZoneMappedTable{rows.first(initial), capacity}};
    std::optional<IndexedTable> rebuilt{ZoneMappedTable{rows.first(initial), capacity}};
    std::int64_t maintain_total = 0, rebuild_total = 0;
    std::size_t merge_total = 0;
    for (std::size_t step = 0; step < steps; ++step) {
      const auto old_size = initial + step * batch_size;
      const auto batch = rows.subspan(old_size, batch_size);
      const auto prefix = rows.first(old_size + batch_size);
      const auto previous = maintained.device_index().merge_count();
      const auto maintain = [&] { maintained.append(batch); };
      const auto rebuild = [&] { rebuilt.emplace(ZoneMappedTable{prefix, capacity}); };
      std::int64_t maintain_ns, rebuild_ns;
      if (sample % 2 == 0) {
        maintain_ns = time_ns(maintain);
        rebuild_ns = time_ns(rebuild);
      } else {
        rebuild_ns = time_ns(rebuild);
        maintain_ns = time_ns(maintain);
      }
      verify(maintained, *rebuilt, prefix);
      const auto merges = maintained.device_index().merge_count() - previous;
      const auto touched = (prefix.size() - 1) / capacity - old_size / capacity + 1;
      maintain_total += maintain_ns;
      rebuild_total += rebuild_ns;
      merge_total += merges;
      if (sample != 0)
        std::cout << scenario << ',' << sample << ",batch," << step << ',' << old_size << ','
                  << batch_size << ',' << touched << ',' << merges << ',' << maintain_ns << ',' << rebuild_ns << '\n';
    }
    if (sample != 0)
      std::cout << scenario << ',' << sample << ",sequence,," << initial << ',' << steps * batch_size
                << ",," << merge_total << ',' << maintain_total << ',' << rebuild_total << '\n';
  }
}
}  // namespace

int main() {
  std::cout << "scenario,sample,scope,step,starting_rows,added_rows,touched_blocks,merges,maintain_ns,rebuild_ns\n";
  compare("single", 1, 128);
  compare("partial", 257, 32);
  compare("multi", 2305, 16);
}
