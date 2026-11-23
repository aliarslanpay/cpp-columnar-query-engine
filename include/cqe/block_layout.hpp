#pragma once

#include <cstddef>

namespace cqe {

// Half-open row range [begin, end) of one block. A block is never empty.
struct BlockRange {
  std::size_t begin = 0;
  std::size_t end = 0;

  [[nodiscard]] std::size_t size() const noexcept { return end - begin; }

  friend bool operator==(const BlockRange&, const BlockRange&) = default;
};

// Logical division of a table's rows into fixed-capacity blocks. Blocks are ranges over the
// existing contiguous columns; they do not change how a store lays out its data.
//
// Block b starts at begin = b * capacity and ends at begin + min(capacity, row_count - begin).
// The blocks tile [0, row_count) without gaps or overlap. Every block has exactly `capacity`
// rows except the last, which has between 1 and `capacity` rows; it is partial when
// row_count % capacity != 0. The empty table has no blocks, so no block is ever empty.
//
// The arithmetic cannot overflow for any row_count and any capacity >= 1, including SIZE_MAX:
// block_count() never forms row_count + capacity - 1, and block() never forms (b + 1) * capacity.
class BlockLayout {
 public:
  static constexpr std::size_t kDefaultCapacity = 1024;

  // Throws std::invalid_argument if capacity == 0.
  BlockLayout(std::size_t row_count, std::size_t capacity);

  [[nodiscard]] std::size_t row_count() const noexcept { return row_count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  // ceil(row_count / capacity); 0 for the empty table.
  [[nodiscard]] std::size_t block_count() const noexcept;

  // The rows of block b. Throws std::out_of_range if b >= block_count().
  [[nodiscard]] BlockRange block(std::size_t b) const;

 private:
  std::size_t row_count_;
  std::size_t capacity_;
};

}  // namespace cqe
