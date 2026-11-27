#pragma once

#include <cstddef>
#include <iterator>
#include <span>

namespace cqe {

// Two ordered, disjoint posting segments. Iteration borrows both buffers.
class PostingView {
 public:
  using Segment = std::span<const std::size_t>;
  PostingView() = default;
  PostingView(Segment base, Segment delta) noexcept : base_(base), delta_(delta) {}

  class iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    iterator() = default;
    reference operator*() const noexcept {
      return pos_ < base_.size() ? base_[pos_] : delta_[pos_ - base_.size()];
    }
    iterator& operator++() noexcept { ++pos_; return *this; }
    iterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
    friend bool operator==(const iterator& a, const iterator& b) noexcept {
      return a.pos_ == b.pos_ && a.base_.data() == b.base_.data() &&
             a.delta_.data() == b.delta_.data();
    }

   private:
    friend class PostingView;
    iterator(Segment base, Segment delta, std::size_t pos) : base_(base), delta_(delta), pos_(pos) {}
    Segment base_, delta_;
    std::size_t pos_ = 0;
  };

  [[nodiscard]] std::size_t size() const noexcept { return base_.size() + delta_.size(); }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] iterator begin() const noexcept { return {base_, delta_, 0}; }
  [[nodiscard]] iterator end() const noexcept { return {base_, delta_, size()}; }

 private:
  Segment base_, delta_;
};

}  // namespace cqe
