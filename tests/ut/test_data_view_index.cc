// Copyright (C) 2019-2023 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "catch2/catch_approx.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "knowhere/bitsetview.h"
#include "knowhere/comp/brute_force.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/comp/knowhere_check.h"
#include "knowhere/comp/knowhere_config.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/log.h"
#include "knowhere/object.h"
#include "simd/hook.h"
#include "utils.h"

namespace {
constexpr float kKnnRecallThreshold = 0.6f;
constexpr float kBruteForceRecallThreshold = 0.95f;
constexpr int kCosineMaxMissNum = 5;
}  // namespace

TEST_CASE("Test SCANN v.s. SCANN with data view refiner", "[float metrics]") {
    using Catch::Approx;
    auto version = GenTestVersionList();
    if (!faiss::support_pq_fast_scan) {
        SKIP("pass scann test");
    }

    const int64_t nb = 1000, nq = 10;
    auto metric = GENERATE(as<std::string>{}, knowhere::metric::COSINE, knowhere::metric::IP, knowhere::metric::L2);
    auto topk = GENERATE(as<int64_t>{}, 5, 120);
    auto dim = GENERATE(as<int64_t>{}, 120);

    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        json[knowhere::meta::RADIUS] = knowhere::IsMetricType(metric, knowhere::metric::L2) ? 10.0 : 0.99;
        json[knowhere::meta::RANGE_FILTER] = knowhere::IsMetricType(metric, knowhere::metric::L2) ? 0.0 : 1.01;
        return json;
    };

    auto scann_gen = [base_gen, topk]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NLIST] = 16;
        json[knowhere::indexparam::NPROBE] = 12;
        json[knowhere::indexparam::REORDER_K] = int(4.0 * topk);
        json[knowhere::indexparam::REFINE_RATIO] = 4.0;
        json[knowhere::indexparam::SUB_DIM] = 2;
        json[knowhere::indexparam::WITH_RAW_DATA] = true;
        json[knowhere::indexparam::ENSURE_TOPK_FULL] = true;
        return json;
    };

    auto rand = GENERATE(1, 2);
    const auto train_ds = GenDataSet(nb, dim, rand);
    const auto query_ds = GenDataSet(nq, dim, rand + 777);

    const knowhere::Json conf = {
        {knowhere::meta::METRIC_TYPE, metric},
        {knowhere::meta::TOPK, topk},
    };
    auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, conf, nullptr);
    knowhere::ViewDataOp data_view = [&train_ds, data_size = sizeof(float) * dim](size_t id) {
        auto data = train_ds->GetTensor();
        return data + data_size * id;
    };
    auto data_view_pack = knowhere::Pack(data_view);
    SECTION("Accuraccy with refine") {
        auto cfg_json = scann_gen().dump();
        knowhere::Json json = knowhere::Json::parse(cfg_json);

        auto scann_with_dv_refiner =
            knowhere::IndexFactory::Instance()
                .Create<knowhere::fp32>(knowhere::IndexEnum::INDEX_FAISS_SCANN_DVR, version, data_view_pack)
                .value();
        auto scann = knowhere::IndexFactory::Instance()
                         .Create<knowhere::fp32>(knowhere::IndexEnum::INDEX_FAISS_SCANN, version)
                         .value();

        REQUIRE(scann_with_dv_refiner.Type() == knowhere::IndexEnum::INDEX_FAISS_SCANN_DVR);
        REQUIRE(scann_with_dv_refiner.Build(train_ds, json) == knowhere::Status::success);
        REQUIRE(scann.Build(train_ds, json) == knowhere::Status::success);
        REQUIRE(scann_with_dv_refiner.Count() == nb);
        REQUIRE(scann_with_dv_refiner.Size() > 0);
        REQUIRE(scann_with_dv_refiner.HasRawData(metric) == false);
        REQUIRE(scann_with_dv_refiner.HasRawData(metric) ==
                knowhere::IndexStaticFaced<knowhere::fp32>::HasRawData(knowhere::IndexEnum::INDEX_FAISS_SCANN_DVR,
                                                                       version, cfg_json));

        SECTION("knn search") {
            auto scann_with_dv_refiner_results = scann_with_dv_refiner.Search(query_ds, json, nullptr);
            auto scann_results = scann.Search(query_ds, json, nullptr);
            REQUIRE(scann_with_dv_refiner_results.has_value());
            REQUIRE(scann_results.has_value());
            float recall1 = GetKNNRecall(*gt.value(), *scann_with_dv_refiner_results.value());
            float recall2 = GetKNNRecall(*gt.value(), *scann_results.value());
            REQUIRE(recall1 == recall2);
            REQUIRE(recall1 > kKnnRecallThreshold);
            REQUIRE(recall2 > kKnnRecallThreshold);

            if (metric == knowhere::metric::COSINE) {
                REQUIRE(CheckDistanceInScope(*scann_with_dv_refiner_results.value(), topk, -1.00001, 1.00001));
            }

            auto scann_with_dv_ids = scann_with_dv_refiner_results.value()->GetIds();
            auto scann_ids = scann_results.value()->GetIds();
            auto scann_with_dv_dis = scann_with_dv_refiner_results.value()->GetDistance();
            auto scann_dis = scann_with_dv_refiner_results.value()->GetDistance();

            if (scann.HasRawData(metric)) {
                if (metric == knowhere::metric::COSINE) {
                    // cosine distances have a little different
                    auto miss_counter = 0;
                    for (auto i = 0; i < nq * topk; i++) {
                        if (scann_with_dv_ids[i] != scann_ids[i]) {
                            miss_counter++;
                        }
                        REQUIRE(std::abs((scann_with_dv_dis[i] - scann_dis[i]) / scann_dis[i]) < 0.00001);
                    }
                    REQUIRE(miss_counter < kCosineMaxMissNum);
                } else {
                    for (auto i = 0; i < nq * topk; i++) {
                        REQUIRE(scann_with_dv_ids[i] == scann_ids[i]);
                        REQUIRE(scann_with_dv_dis[i] == scann_dis[i]);
                    }
                }
            }
        }

        SECTION("range search") {
            auto scann_results = scann.RangeSearch(query_ds, json, nullptr);
            auto scann_with_dv_refiner_results = scann_with_dv_refiner.RangeSearch(query_ds, json, nullptr);
            REQUIRE(scann_with_dv_refiner_results.has_value() & scann_results.has_value());
            auto scann_with_dv_ids = scann_with_dv_refiner_results.value()->GetIds();
            auto scann_with_dv_lims = scann_with_dv_refiner_results.value()->GetLims();
            auto scann_ids = scann_results.value()->GetIds();
            auto scann_lims = scann_results.value()->GetLims();
            if (scann.HasRawData(metric)) {
                for (auto i = 1; i < nq + 1; i++) {
                    REQUIRE(scann_lims[i] == scann_with_dv_lims[i]);
                }
                for (size_t i = 0; i < scann_lims[nq]; i++) {
                    REQUIRE(scann_with_dv_ids[i] == scann_ids[i]);
                }
            }
        }

        SECTION("knn search with bitset") {
            std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
                GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
            const auto bitset_percentages = {0.22f, 0.98f};
            for (const float percentage : bitset_percentages) {
                for (const auto& gen_func : gen_bitset_funcs) {
                    auto bitset_data = gen_func(nb, percentage * nb);
                    knowhere::BitsetView bitset(bitset_data.data(), nb);
                    auto scann_with_dv_refiner_results = scann_with_dv_refiner.Search(query_ds, json, bitset);
                    auto scann_results = scann.Search(query_ds, json, bitset);
                    auto gt_with_filter =
                        knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, json, bitset);
                    REQUIRE(scann_results.has_value() & scann_with_dv_refiner_results.has_value());
                    float recall1 = GetKNNRecall(*gt_with_filter.value(), *scann_with_dv_refiner_results.value());
                    float recall2 = GetKNNRecall(*gt_with_filter.value(), *scann_results.value());
                    REQUIRE(recall1 == recall2);
                    REQUIRE(recall1 > kKnnRecallThreshold);
                    REQUIRE(recall2 > kKnnRecallThreshold);
                    if (metric == knowhere::metric::COSINE) {
                        REQUIRE(CheckDistanceInScope(*scann_with_dv_refiner_results.value(), topk, -1.00001, 1.00001));
                    }
                }
            }
        }
    }
}

template <typename DataType>
void
BaseTest(const knowhere::DataSetPtr train_ds, const knowhere::DataSetPtr query_ds, const int64_t k,
         const knowhere::MetricType metric, const knowhere::Json& conf) {
    auto version = knowhere::Version::GetCurrentVersion().VersionNumber();
    auto base = knowhere::ConvertToDataTypeIfNeeded<DataType>(train_ds);
    auto query = knowhere::ConvertToDataTypeIfNeeded<DataType>(query_ds);
    auto dim = base->GetDim();
    auto nb = base->GetRows();
    auto nq = query->GetRows();

    auto knn_gt = knowhere::BruteForce::Search<DataType>(base, query, conf, nullptr);
    knowhere::ViewDataOp data_view = [&base, data_size = sizeof(DataType) * dim](size_t id) {
        auto data = base->GetTensor();
        return data + data_size * id;
    };
    auto data_view_pack = knowhere::Pack(data_view);
    auto scann_with_dv_refiner =
        knowhere::IndexFactory::Instance()
            .Create<DataType>(knowhere::IndexEnum::INDEX_FAISS_SCANN_DVR, version, data_view_pack)
            .value();

    REQUIRE(scann_with_dv_refiner.Type() == knowhere::IndexEnum::INDEX_FAISS_SCANN_DVR);
    REQUIRE(scann_with_dv_refiner.Build(base, conf) == knowhere::Status::success);

    REQUIRE(scann_with_dv_refiner.Size() > 0);
    REQUIRE(scann_with_dv_refiner.HasRawData(metric) == false);
    REQUIRE(scann_with_dv_refiner.HasRawData(metric) == knowhere::IndexStaticFaced<knowhere::fp32>::HasRawData(
                                                            knowhere::IndexEnum::INDEX_FAISS_SCANN_DVR, version, conf));

    // knn search
    auto scann_with_dv_refiner_results = scann_with_dv_refiner.Search(query, conf, nullptr);
    REQUIRE(scann_with_dv_refiner_results.has_value());
    float recall = GetKNNRecall(*knn_gt.value(), *scann_with_dv_refiner_results.value());
    REQUIRE(recall > kKnnRecallThreshold);
    if (metric == knowhere::metric::COSINE) {
        REQUIRE(CheckDistanceInScope(*scann_with_dv_refiner_results.value(), k, -1.00001, 1.00001));
    }
    // range search
    auto scann_with_dv_refiner_range_results = scann_with_dv_refiner.RangeSearch(query, conf, nullptr);
    REQUIRE(scann_with_dv_refiner_range_results.has_value());
    auto scann_with_dv_ids = scann_with_dv_refiner_range_results.value()->GetIds();
    auto scann_with_dv_lims = scann_with_dv_refiner_range_results.value()->GetLims();
    if (metric == knowhere::metric::L2 || metric == knowhere::metric::COSINE) {
        for (int i = 0; i < nq; ++i) {
            CHECK(scann_with_dv_ids[scann_with_dv_lims[i]] == i);
        }
    }
}

TEST_CASE("Test difference dim with difference data type", "[multi metrics]") {
    if (!faiss::support_pq_fast_scan) {
        SKIP("pass scann test");
    }
    const int64_t nb = 1000, nq = 10;
    auto metric = GENERATE(as<std::string>{}, knowhere::metric::COSINE, knowhere::metric::IP, knowhere::metric::L2);
    auto topk = GENERATE(as<int64_t>{}, 10);
    auto dim = GENERATE(as<int64_t>{}, 31, 128, 511, 1024);

    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        json[knowhere::meta::RADIUS] = knowhere::IsMetricType(metric, knowhere::metric::L2) ? 10.0 : 0.9;
        json[knowhere::meta::RANGE_FILTER] = knowhere::IsMetricType(metric, knowhere::metric::L2) ? 0.0 : 1.01;
        return json;
    };

    auto scann_gen = [base_gen, topk]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NLIST] = 24;
        json[knowhere::indexparam::NPROBE] = 16;
        json[knowhere::indexparam::REFINE_RATIO] = 4.0;
        json[knowhere::indexparam::SUB_DIM] = 2;
        return json;
    };

    const auto train_ds = GenDataSet(nb, dim);
    const auto query_ds = GenDataSet(nq, dim);
    auto cfg_json = scann_gen().dump();
    knowhere::Json json = knowhere::Json::parse(cfg_json);
    BaseTest<knowhere::fp32>(train_ds, query_ds, topk, metric, json);
    BaseTest<knowhere::bf16>(train_ds, query_ds, topk, metric, json);
    BaseTest<knowhere::fp16>(train_ds, query_ds, topk, metric, json);
}
