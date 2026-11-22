#pragma once

#include <span>

#include "cqe/types.hpp"

namespace cqe {

// Reference evaluator: the oracle that every execution mode is compared against.
//
// A deliberately trivial row-wise loop over the rows as generated. It depends only on types.hpp
// and shares no code with storage, metadata, indexes or kernels, so that a bug in shared code
// cannot hide in both the result and the expected value.
//
// Precondition: `rows` satisfy the amount-total invariant (sum_bound.hpp). The reference does not
// check it, because the check is storage code; UBSan reports a violation in sanitizer builds.
[[nodiscard]] QueryResult reference_evaluate(std::span<const Event> rows, const Query& query);

}  // namespace cqe
