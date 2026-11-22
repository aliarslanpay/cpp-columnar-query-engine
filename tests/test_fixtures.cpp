// Hand-computed expected results on small fixtures. Every fixture runs against the reference
// evaluator and against both full-scan executors, so the reference itself is checked against
// values worked out by hand, not only against the other implementations.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cqe/column_store.hpp"
#include "cqe/executor.hpp"
#include "cqe/reference.hpp"
#include "cqe/row_store.hpp"
#include "test_util.hpp"

namespace cqe {
namespace {

using test::device;
using test::kInt32Max;
using test::kInt32Min;
using test::kInt64Max;
using test::kInt64Min;
using test::range;

enum class Evaluator { Reference, RowStoreFullScan, ColumnStoreFullScan };

class Fixture : public ::testing::TestWithParam<Evaluator> {
 protected:
  QueryResult run(const std::vector<Event>& rows, const Query& query) const {
    switch (GetParam()) {
      case Evaluator::Reference:
        return reference_evaluate(rows, query);
      case Evaluator::RowStoreFullScan:
        return full_scan(RowStore{rows}, query);
      case Evaluator::ColumnStoreFullScan:
        return full_scan(ColumnStore{rows}, query);
    }
    ADD_FAILURE() << "unknown evaluator";
    return {};
  }
};

TEST_P(Fixture, EmptyTable) {
  const std::vector<Event> rows;
  EXPECT_EQ(run(rows, range(0, 10)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, range(kInt64Min, kInt64Max)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, device(0)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, device(kInt32Min)), (QueryResult{0, 0}));
}

TEST_P(Fixture, OneRow) {
  const std::vector<Event> rows = {{10, 7, -5}};
  EXPECT_EQ(run(rows, range(10, 10)), (QueryResult{1, -5}));
  EXPECT_EQ(run(rows, range(kInt64Min, kInt64Max)), (QueryResult{1, -5}));
  EXPECT_EQ(run(rows, range(11, 20)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, range(0, 9)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, device(7)), (QueryResult{1, -5}));
  EXPECT_EQ(run(rows, device(8)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, device(-7)), (QueryResult{0, 0}));
}

TEST_P(Fixture, Duplicates) {
  // Three identical rows, a fourth row with the same timestamp and another device, and a fifth
  // row with the first device and another timestamp.
  const std::vector<Event> rows = {{5, 1, 3}, {5, 1, 3}, {5, 1, 3}, {5, 2, 10}, {6, 1, -1}};
  EXPECT_EQ(run(rows, device(1)), (QueryResult{4, 8}));    // 3 + 3 + 3 - 1
  EXPECT_EQ(run(rows, device(2)), (QueryResult{1, 10}));
  EXPECT_EQ(run(rows, range(5, 5)), (QueryResult{4, 19}));  // 3 + 3 + 3 + 10
  EXPECT_EQ(run(rows, range(6, 6)), (QueryResult{1, -1}));
  EXPECT_EQ(run(rows, range(5, 6)), (QueryResult{5, 18}));
}

TEST_P(Fixture, NegativeAmounts) {
  const std::vector<Event> rows = {{1, 1, -10}, {2, 1, -20}, {3, 2, 7}, {4, 2, -7}};
  EXPECT_EQ(run(rows, range(1, 2)), (QueryResult{2, -30}));
  EXPECT_EQ(run(rows, range(1, 4)), (QueryResult{4, -30}));
  EXPECT_EQ(run(rows, range(3, 3)), (QueryResult{1, 7}));
  EXPECT_EQ(run(rows, device(1)), (QueryResult{2, -30}));
  // Matching rows whose amounts cancel: sum 0 but count 2, which is not the empty result.
  EXPECT_EQ(run(rows, device(2)), (QueryResult{2, 0}));
  EXPECT_NE(run(rows, device(2)), (QueryResult{0, 0}));
}

TEST_P(Fixture, LoGreaterThanHiIsAnEmptyQuery) {
  // Timestamps 10..20, amounts 1..11.
  std::vector<Event> rows;
  for (std::int64_t i = 0; i <= 10; ++i) {
    rows.push_back(Event{10 + i, static_cast<std::int32_t>(i % 3), i + 1});
  }
  EXPECT_EQ(run(rows, range(10, 20)), (QueryResult{11, 66}));  // control: 1 + ... + 11
  EXPECT_EQ(run(rows, range(20, 10)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, range(11, 10)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, range(15, 14)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, range(kInt64Max, kInt64Min)), (QueryResult{0, 0}));
}

TEST_P(Fixture, BoundsEqualToLoAndHiAreIncluded) {
  const std::vector<Event> rows = {
      {99, 0, 1}, {100, 0, 10}, {150, 0, 100}, {200, 0, 1000}, {201, 0, 10000}};
  EXPECT_EQ(run(rows, range(100, 200)), (QueryResult{3, 1110}));
  EXPECT_EQ(run(rows, range(101, 199)), (QueryResult{1, 100}));
  EXPECT_EQ(run(rows, range(99, 201)), (QueryResult{5, 11111}));
  EXPECT_EQ(run(rows, range(100, 100)), (QueryResult{1, 10}));  // lo == hi
  EXPECT_EQ(run(rows, range(200, 200)), (QueryResult{1, 1000}));
  EXPECT_EQ(run(rows, range(202, 300)), (QueryResult{0, 0}));
  EXPECT_EQ(run(rows, range(0, 98)), (QueryResult{0, 0}));
}

TEST_P(Fixture, ExtremeColumnValues) {
  const std::vector<Event> rows = {
      {kInt64Min, kInt32Min, 1}, {kInt64Max, kInt32Max, 2}, {0, 0, 4}};
  EXPECT_EQ(run(rows, range(kInt64Min, kInt64Min)), (QueryResult{1, 1}));
  EXPECT_EQ(run(rows, range(kInt64Max, kInt64Max)), (QueryResult{1, 2}));
  EXPECT_EQ(run(rows, range(kInt64Min, kInt64Max)), (QueryResult{3, 7}));
  EXPECT_EQ(run(rows, range(kInt64Min + 1, kInt64Max - 1)), (QueryResult{1, 4}));
  EXPECT_EQ(run(rows, device(kInt32Min)), (QueryResult{1, 1}));
  EXPECT_EQ(run(rows, device(kInt32Max)), (QueryResult{1, 2}));
  EXPECT_EQ(run(rows, device(0)), (QueryResult{1, 4}));
}

TEST_P(Fixture, SumsAtTheInt64Limits) {
  // INT64_MAX / 4 = 2'305'843'009'213'693'951; four of them sum to 9'223'372'036'854'775'804.
  const std::int64_t quarter_max = 2'305'843'009'213'693'951;
  const std::vector<Event> positive = {
      {0, 0, quarter_max}, {1, 0, quarter_max}, {2, 0, quarter_max}, {3, 0, quarter_max}};
  EXPECT_EQ(run(positive, range(kInt64Min, kInt64Max)),
            (QueryResult{4, 9'223'372'036'854'775'804}));

  // INT64_MIN / 4 = -2'305'843'009'213'693'952; four of them sum to exactly INT64_MIN.
  const std::int64_t quarter_min = -2'305'843'009'213'693'952;
  const std::vector<Event> negative = {
      {0, 0, quarter_min}, {1, 0, quarter_min}, {2, 0, quarter_min}, {3, 0, quarter_min}};
  EXPECT_EQ(run(negative, range(kInt64Min, kInt64Max)), (QueryResult{4, kInt64Min}));

  // P = INT64_MAX and N = INT64_MIN in one table: INT64_MAX + INT64_MIN = -1.
  const std::vector<Event> both = {{0, 0, kInt64Max}, {1, 1, kInt64Min}};
  EXPECT_EQ(run(both, range(kInt64Min, kInt64Max)), (QueryResult{2, -1}));
  EXPECT_EQ(run(both, range(0, 0)), (QueryResult{1, kInt64Max}));
  EXPECT_EQ(run(both, range(1, 1)), (QueryResult{1, kInt64Min}));
  EXPECT_EQ(run(both, device(1)), (QueryResult{1, kInt64Min}));
}

INSTANTIATE_TEST_SUITE_P(AllEvaluators, Fixture,
                         ::testing::Values(Evaluator::Reference, Evaluator::RowStoreFullScan,
                                           Evaluator::ColumnStoreFullScan),
                         [](const ::testing::TestParamInfo<Evaluator>& info) -> std::string {
                           switch (info.param) {
                             case Evaluator::Reference:
                               return "Reference";
                             case Evaluator::RowStoreFullScan:
                               return "RowStoreFullScan";
                             case Evaluator::ColumnStoreFullScan:
                               return "ColumnStoreFullScan";
                           }
                           return "Unknown";
                         });

}  // namespace
}  // namespace cqe
