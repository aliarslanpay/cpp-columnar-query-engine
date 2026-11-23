#include "cqe/block_layout.hpp"

#include <algorithm>
#include <stdexcept>

namespace cqe {

BlockLayout::BlockLayout(std::size_t row_count, std::size_t capacity)
    : row_count_(row_count), capacity_(capacity) {
  if (capacity == 0) {
    throw std::invalid_argument("BlockLayout: block capacity must be >= 1");
  }
}

std::size_t BlockLayout::block_count() const noexcept {
  return row_count_ / capacity_ + (row_count_ % capacity_ != 0 ? std::size_t{1} : std::size_t{0});
}

BlockRange BlockLayout::block(std::size_t b) const {
  if (b >= block_count()) {
    throw std::out_of_range("BlockLayout::block: block index out of range");
  }
  // b <= block_count() - 1 implies b * capacity_ < row_count_, so the product cannot overflow,
  // and row_count_ - begin >= 1.
  const std::size_t begin = b * capacity_;
  const std::size_t end = begin + std::min(capacity_, row_count_ - begin);
  return BlockRange{begin, end};
}

}  // namespace cqe
