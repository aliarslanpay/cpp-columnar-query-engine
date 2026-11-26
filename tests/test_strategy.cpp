#include <gtest/gtest.h>

#include "cqe/strategy.hpp"

namespace cqe {
namespace {

TEST(ChooseStrategy, EmptyTableUsesOneWorkerAndAnEligibleStrategy) {
  const ExecutionPlan plan = choose_strategy({0, 0, {}}, 8);
  EXPECT_NE(plan.strategy, ScanStrategy::Index);
  EXPECT_EQ(plan.threads, 1);
}

TEST(ChooseStrategy, RangeWithNoCandidatesChoosesZoneMapSerially) {
  const ExecutionPlan plan = choose_strategy({100, 0, {}}, 8);
  EXPECT_EQ(plan.strategy, ScanStrategy::ZoneMap);
  EXPECT_EQ(plan.threads, 1);
}

TEST(ChooseStrategy, EqualityWithNoIndexCandidatesChoosesIndexSerially) {
  const ExecutionPlan plan = choose_strategy({100, 100, 0}, 8);
  EXPECT_EQ(plan.strategy, ScanStrategy::Index);
  EXPECT_EQ(plan.threads, 1);
}

TEST(ChooseStrategy, SingleWorkerBudgetAndMissingIndexAreRespected) {
  const ExecutionPlan plan = choose_strategy({1'000'000, 900'000, {}}, 1);
  EXPECT_NE(plan.strategy, ScanStrategy::Index);
  EXPECT_EQ(plan.threads, 1);
}

TEST(ChooseStrategy, SingleBlockCannotUseMultipleWorkers) {
  const ExecutionPlan plan = choose_strategy({1, 1, 1}, 8);
  EXPECT_EQ(plan.threads, 1);
}

TEST(ChooseStrategy, DenseScanUsesMultipleWorkersWhenSetupCostIsAmortized) {
  const ExecutionPlan plan = choose_strategy({1'000'000, 1'000'000, {}}, 4);
  EXPECT_NE(plan.strategy, ScanStrategy::Index);
  EXPECT_GT(plan.threads, 1);
  EXPECT_LE(plan.threads, 4);
}

TEST(ChooseStrategy, SingleMatchingBlockWinsOverDenseAlternatives) {
  const ExecutionPlan indexed = choose_strategy({128, 128, 1}, 8);
  EXPECT_EQ(indexed.strategy, ScanStrategy::Index);
  EXPECT_EQ(indexed.threads, 1);
  const ExecutionPlan zoned = choose_strategy({128, 1, {}}, 8);
  EXPECT_EQ(zoned.strategy, ScanStrategy::ZoneMap);
  EXPECT_EQ(zoned.threads, 1);
}

TEST(ChooseStrategy, PartialThreadBudgetAllowsSmallerParallelTeams) {
  const ExecutionPlan plan = choose_strategy({128, 128, 128}, 3);
  EXPECT_GT(plan.threads, 1);
  EXPECT_LE(plan.threads, 3);
}

}  // namespace
}  // namespace cqe
