#pragma once

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace cqe::detail {

// Allocate away from live storage. An empty replacement means the live buffer already fits.
template<class T>
std::vector<T> prepare_growth(const std::vector<T>& live, std::size_t extra) {
  if (extra > live.max_size() - live.size()) {
    throw std::length_error("append exceeds vector size limit");
  }
  std::vector<T> next;
  const auto needed = live.size() + extra;
  if (needed > live.capacity()) {
    const auto capacity = live.capacity();
    next.reserve(std::max(needed, capacity + std::min(capacity, live.max_size() - capacity)));
    next.insert(next.end(), live.begin(), live.end());
  }
  return next;
}

template<class T>
void publish_growth(std::vector<T>& live, std::vector<T>& prepared) noexcept {
  if (prepared.capacity() != 0) live.swap(prepared);
}

}  // namespace cqe::detail
