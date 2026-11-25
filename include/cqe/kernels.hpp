#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace cqe {

// Offset of a row inside the column slice (one block) that a selection kernel scans.
// 32 bits are enough for a slice of up to 2^32 rows: its last offset, 2^32 - 1, is UINT32_MAX.
using RowOffset = std::uint32_t;

// The largest slice a selection kernel accepts.
inline constexpr std::uint64_t kMaxSelectableRows = std::uint64_t{1} << 32;

// Selection kernels (scalar). Each one scans a column slice and writes the offsets of the rows
// that satisfy the predicate to the front of `out`, in strictly ascending order, and returns how
// many it wrote.
//
// Preconditions:
//   - column.size() <= kMaxSelectableRows, so every offset fits in RowOffset;
//   - out.size() >= column.size(), because every row may match. The kernels may also write to
//     out[k], ..., out[column.size() - 1] beyond the returned count k; those entries are
//     unspecified afterwards. Entries at index column.size() and above are never written.
// Postcondition, for the returned k: k <= column.size(), and out[0..k) are exactly the offsets i
// of the matching rows, each with i < column.size(), in strictly ascending order.
//
// The predicates are the closed ones of types.hpp: lo <= value <= hi (lo > hi matches nothing),
// and value == v.
[[nodiscard]] std::size_t select_range(std::span<const std::int64_t> column, std::int64_t lo,
                                       std::int64_t hi, std::span<RowOffset> out) noexcept;
[[nodiscard]] std::size_t select_equals(std::span<const std::int32_t> column, std::int32_t value,
                                        std::span<RowOffset> out) noexcept;

// Late materialization: the sum of column[s] over the offsets s in `selection`, reading the
// column only at those offsets.
// Precondition: every s in `selection` is < column.size(). A selection produced by the kernels
// above for the same slice satisfies this. The amount-total invariant (sum_bound.hpp) keeps the
// sum in range for any selection without duplicates.
[[nodiscard]] std::int64_t sum_selected(std::span<const std::int64_t> column,
                                        std::span<const RowOffset> selection) noexcept;

}  // namespace cqe
