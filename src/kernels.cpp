#include "cqe/kernels.hpp"

#include <cassert>

namespace cqe {

// Both selection kernels write the current offset unconditionally and advance the output
// position by the match result, so the source has no data-dependent branch. This describes the
// source only; the generated machine code has not been inspected.

std::size_t select_range(std::span<const std::int64_t> column, std::int64_t lo, std::int64_t hi,
                         std::span<RowOffset> out) noexcept {
  assert(column.size() <= kMaxSelectableRows);
  assert(out.size() >= column.size());
  std::size_t k = 0;
  for (std::size_t i = 0; i < column.size(); ++i) {
    const std::int64_t v = column[i];
    out[k] = static_cast<RowOffset>(i);  // k <= i < column.size() <= out.size()
    k += static_cast<std::size_t>((lo <= v) & (v <= hi));
  }
  return k;
}

std::size_t select_equals(std::span<const std::int32_t> column, std::int32_t value,
                          std::span<RowOffset> out) noexcept {
  assert(column.size() <= kMaxSelectableRows);
  assert(out.size() >= column.size());
  std::size_t k = 0;
  for (std::size_t i = 0; i < column.size(); ++i) {
    out[k] = static_cast<RowOffset>(i);
    k += static_cast<std::size_t>(column[i] == value);
  }
  return k;
}

std::int64_t sum_selected(std::span<const std::int64_t> column,
                          std::span<const RowOffset> selection) noexcept {
  std::int64_t sum = 0;
  for (const RowOffset s : selection) {
    assert(s < column.size());
    sum += column[s];
  }
  return sum;
}

}  // namespace cqe
