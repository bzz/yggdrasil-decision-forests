/*
 * Copyright 2022 Google LLC.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/loss/loss_imp_ndcg.h"

#include "gmock/gmock.h"
#include "yggdrasil_decision_forests/dataset/vertical_dataset.h"
#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/gradient_boosted_trees.h"
#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/loss/loss_imp_cross_entropy_ndcg.h"
#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/loss/loss_interface.h"
#include "yggdrasil_decision_forests/model/abstract_model.pb.h"
#include "yggdrasil_decision_forests/utils/test.h"
#include "yggdrasil_decision_forests/utils/testing_macros.h"

namespace yggdrasil_decision_forests {
namespace model {
namespace gradient_boosted_trees {
namespace {

// Margin of error for numerical tests. Note that this is by a factor of 10
// larger than for the other loss functions.
constexpr float kTestPrecision = 0.00001f;

using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::IsEmpty;
using ::testing::SizeIs;

// TODO: Improve testing coverage for NDCG loss functions.

absl::StatusOr<dataset::VerticalDataset> CreateToyDataset() {
  dataset::VerticalDataset dataset;
  // TODO Replace by a modern function when possible.
  *dataset.mutable_data_spec() = PARSE_TEST_PROTO(R"pb(
    columns { type: NUMERICAL name: "a" }
    columns {
      type: CATEGORICAL
      name: "b"
      categorical { number_of_unique_values: 3 is_already_integerized: true }
    }
  )pb");
  RETURN_IF_ERROR(dataset.CreateColumnsFromDataspec());
  RETURN_IF_ERROR(dataset.AppendExampleWithStatus({{"a", "1"}, {"b", "1"}}));
  RETURN_IF_ERROR(dataset.AppendExampleWithStatus({{"a", "2"}, {"b", "2"}}));
  RETURN_IF_ERROR(dataset.AppendExampleWithStatus({{"a", "3"}, {"b", "1"}}));
  RETURN_IF_ERROR(dataset.AppendExampleWithStatus({{"a", "4"}, {"b", "2"}}));
  return dataset;
}

TEST(NDCGLossTest, RankingIndexInitialization) {
  ASSERT_OK_AND_ASSIGN(const dataset::VerticalDataset dataset,
                       CreateToyDataset());
  std::vector<float> weights = {1.f, 1.f, 1.f, 1.f};

  RankingGroupsIndices index;
  index.Initialize(dataset, 0, 1);
  ASSERT_THAT(index.groups(), SizeIs(2));
  ASSERT_THAT(index.groups()[0].items, SizeIs(2));
  ASSERT_THAT(index.groups()[1].items, SizeIs(2));
  EXPECT_EQ(index.groups()[0].items[0].example_idx, 2);
  EXPECT_EQ(index.groups()[0].items[0].relevance, 3);
  EXPECT_EQ(index.groups()[0].items[1].example_idx, 0);
  EXPECT_EQ(index.groups()[0].items[1].relevance, 1);
  EXPECT_EQ(index.groups()[1].items[0].example_idx, 3);
  EXPECT_EQ(index.groups()[1].items[0].relevance, 4);
  EXPECT_EQ(index.groups()[1].items[1].example_idx, 1);
  EXPECT_EQ(index.groups()[1].items[1].relevance, 2);
}

TEST(NDCGLossTest, PerfectPrediction) {
  // Dataset containing two groups with relevance {1,3} and {2,4} respectively.
  ASSERT_OK_AND_ASSIGN(const dataset::VerticalDataset dataset,
                       CreateToyDataset());
  std::vector<float> weights = {1.f, 1.f, 1.f, 1.f};
  RankingGroupsIndices index;
  index.Initialize(dataset, 0, 1);

  // This is a perfect prediction.
  double perfect_prediction = index.NDCG({10, 11, 12, 13}, weights, 5);
  EXPECT_NEAR(perfect_prediction, 1., kTestPrecision);

  // This is another perfect predictions (the ranking across groups has no
  // effect).
  double prefect_prediction_again = index.NDCG({10, 11, 12, 13}, weights, 5);
  EXPECT_NEAR(prefect_prediction_again, 1., kTestPrecision);
}

TEST(NDCGLossTest, PerfectlyWrongPrediction) {
  // Dataset containing two groups with relevance {1,3} and {2,4} respectively.
  ASSERT_OK_AND_ASSIGN(const dataset::VerticalDataset dataset,
                       CreateToyDataset());
  std::vector<float> weights = {1.f, 1.f, 1.f, 1.f};
  RankingGroupsIndices index;
  index.Initialize(dataset, 0, 1);

  // Perfectly wrong predictions.
  // R> 0.7238181 = (sum((2^c(1,3)-1)/log2(seq(2)+1)) /
  // sum((2^c(3,1)-1)/log2(seq(2)+1)) +  sum((2^c(2,4)-1)/log2(seq(2)+1)) /
  // sum((2^c(4,2)-1)/log2(seq(2)+1)) )/2
  double prefectly_wrong_prediction = index.NDCG({2, 2, 1, 1}, weights, 5);
  EXPECT_NEAR(prefectly_wrong_prediction, 0.723818, kTestPrecision);
}

TEST(NDCGLossTest, UpdateGradients) {
  ASSERT_OK_AND_ASSIGN(const dataset::VerticalDataset dataset,
                       CreateToyDataset());
  std::vector<float> weights = {1.f, 1.f, 1.f, 1.f};

  RankingGroupsIndices index;
  index.Initialize(dataset, 0, 1);
  EXPECT_THAT(index.groups(), SizeIs(2));

  dataset::VerticalDataset gradient_dataset;
  std::vector<GradientData> gradients;
  std::vector<float> predictions;
  const NDCGLoss loss_imp({}, model::proto::Task::RANKING,
                          dataset.data_spec().columns(0));
  ASSERT_OK(internal::CreateGradientDataset(dataset,
                                            /* label_col_idx= */ 0,
                                            /*hessian_splits=*/false, loss_imp,
                                            &gradient_dataset, &gradients,
                                            &predictions));
  ASSERT_OK_AND_ASSIGN(
      const std::vector<float> initial_predictions,
      loss_imp.InitialPredictions(dataset, /* label_col_idx =*/0, weights));
  internal::SetInitialPredictions(initial_predictions, dataset.nrow(),
                                  &predictions);

  utils::RandomEngine random(1234);
  ASSERT_OK(loss_imp.UpdateGradients(gradient_dataset,
                                     /* label_col_idx= */ 0, predictions,
                                     &index, &gradients, &random));

  ASSERT_THAT(gradients, Not(IsEmpty()));
  const std::vector<float>& gradient = gradients.front().gradient;
  // Explanation:
  // - Element 0 is pushed down by element 2 (and in reverse).
  // - Element 1 is pushed down by element 3 (and in reverse).
  EXPECT_THAT(gradient, ElementsAre(FloatNear(-0.14509f, kTestPrecision),
                                    FloatNear(-0.13109f, kTestPrecision),
                                    FloatNear(0.14509, kTestPrecision),
                                    FloatNear(0.13109, kTestPrecision)));
}

TEST(NDCGLossTest, UpdateGradientsXeNDCGMart) {
  ASSERT_OK_AND_ASSIGN(const dataset::VerticalDataset dataset,
                       CreateToyDataset());
  std::vector<float> weights = {1.f, 1.f, 1.f, 1.f};

  RankingGroupsIndices index;
  index.Initialize(dataset, 0, 1);
  EXPECT_THAT(index.groups(), SizeIs(2));

  dataset::VerticalDataset gradient_dataset;
  std::vector<GradientData> gradients;
  std::vector<float> predictions;
  const CrossEntropyNDCGLoss loss_imp({}, model::proto::Task::RANKING,
                                      dataset.data_spec().columns(0));
  ASSERT_OK(internal::CreateGradientDataset(dataset,
                                            /* label_col_idx= */ 0,
                                            /*hessian_splits=*/false, loss_imp,
                                            &gradient_dataset, &gradients,
                                            &predictions));

  ASSERT_OK_AND_ASSIGN(
      const std::vector<float> initial_predictions,
      loss_imp.InitialPredictions(dataset, /* label_col_idx =*/0, weights));
  internal::SetInitialPredictions(initial_predictions, dataset.nrow(),
                                  &predictions);

  utils::RandomEngine random(1234);
  ASSERT_OK(loss_imp.UpdateGradients(gradient_dataset,
                                     /* label_col_idx= */ 0, predictions,
                                     &index, &gradients, &random));

  ASSERT_THAT(gradients, Not(IsEmpty()));
  const std::vector<float>& gradient = gradients.front().gradient;
  // Explanation:
  // - Element 0 is pushed down by element 2 (and in reverse).
  // - Element 1 is pushed down by element 3 (and in reverse).
  EXPECT_THAT(gradient, ElementsAre(FloatNear(-0.33864f, kTestPrecision),
                                    FloatNear(-0.32854f, kTestPrecision),
                                    FloatNear(0.33864f, kTestPrecision),
                                    FloatNear(0.32854f, kTestPrecision)));
}

}  // namespace
}  // namespace gradient_boosted_trees
}  // namespace model
}  // namespace yggdrasil_decision_forests
