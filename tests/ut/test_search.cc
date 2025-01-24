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
#include "faiss/utils/binary_distances.h"
#include "hnswlib/hnswalg.h"
#include "knowhere/bitsetview.h"
#include "knowhere/comp/brute_force.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/comp/knowhere_check.h"
#include "knowhere/comp/knowhere_config.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/log.h"
#include "simd/hook.h"
#include "utils.h"

namespace {
constexpr float kKnnRecallThreshold = 0.6f;
constexpr float kBruteForceRecallThreshold = 0.95f;
constexpr const char* kMmapIndexPath = "/tmp/knowhere_dense_mmap_index_test";
}  // namespace

TEST_CASE("Test Mem Index With Float Vector", "[float metrics]") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;
    const int64_t dim = 128;

    auto metric = GENERATE(as<std::string>{}, knowhere::metric::L2, knowhere::metric::COSINE);
    auto topk = GENERATE(as<int64_t>{}, 5, 120);
    auto version = GenTestVersionList();

    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        json[knowhere::meta::RADIUS] = knowhere::IsMetricType(metric, knowhere::metric::L2) ? 10.0 : 0.99;
        json[knowhere::meta::RANGE_FILTER] = knowhere::IsMetricType(metric, knowhere::metric::L2) ? 0.0 : 1.01;
        return json;
    };

    auto ivfflat_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NLIST] = 16;
        json[knowhere::indexparam::NPROBE] = 8;
        return json;
    };

    auto ivfflatcc_gen = [ivfflat_gen]() {
        knowhere::Json json = ivfflat_gen();
        json[knowhere::indexparam::SSIZE] = 48;
        return json;
    };

    auto ivfsqcc_code_size_4_gen = [ivfflatcc_gen]() {
        knowhere::Json json = ivfflatcc_gen();
        json[knowhere::indexparam::SSIZE] = 48;
        json[knowhere::indexparam::CODE_SIZE] = 4;
        return json;
    };

    auto ivfsqcc_code_size_6_gen = [ivfflatcc_gen]() {
        knowhere::Json json = ivfflatcc_gen();
        json[knowhere::indexparam::SSIZE] = 48;
        json[knowhere::indexparam::CODE_SIZE] = 6;
        return json;
    };

    auto ivfsqcc_code_size_8_gen = [ivfflatcc_gen]() {
        knowhere::Json json = ivfflatcc_gen();
        json[knowhere::indexparam::SSIZE] = 48;
        json[knowhere::indexparam::CODE_SIZE] = 8;
        return json;
    };

    auto ivfsqcc_code_size_16_gen = [ivfflatcc_gen]() {
        knowhere::Json json = ivfflatcc_gen();
        json[knowhere::indexparam::SSIZE] = 48;
        json[knowhere::indexparam::CODE_SIZE] = 16;
        return json;
    };

    auto ivfsq_gen = ivfflat_gen;

    auto flat_gen = base_gen;

    auto ivfpq_gen = [ivfflat_gen]() {
        knowhere::Json json = ivfflat_gen();
        json[knowhere::indexparam::M] = 4;
        json[knowhere::indexparam::NBITS] = 8;
        return json;
    };

    auto scann_gen = [ivfflat_gen]() {
        knowhere::Json json = ivfflat_gen();
        json[knowhere::indexparam::NPROBE] = 14;
        json[knowhere::indexparam::REORDER_K] = 200;
        json[knowhere::indexparam::WITH_RAW_DATA] = true;
        return json;
    };

    auto scann_gen2 = [scann_gen]() {
        knowhere::Json json = scann_gen();
        json[knowhere::indexparam::WITH_RAW_DATA] = false;
        return json;
    };

    auto hnsw_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::HNSW_M] = 32;
        json[knowhere::indexparam::EFCONSTRUCTION] = 120;
        json[knowhere::indexparam::EF] = 120;
        return json;
    };

    auto ordered_rs_hnsw_gen = [=]() {
        knowhere::Json json = hnsw_gen();
        json[knowhere::meta::RANGE_SEARCH_K] = topk;
        json[knowhere::meta::RETAIN_ITERATOR_ORDER] = true;
        if (knowhere::IsMetricType(metric, knowhere::metric::L2)) {
            json[knowhere::meta::RANGE_FILTER] = 0.0f;
            json[knowhere::meta::RADIUS] = 1000000.0f;
        } else if (knowhere::IsMetricType(metric, knowhere::metric::COSINE)) {
            json[knowhere::meta::RANGE_FILTER] = 1.0f;
            json[knowhere::meta::RADIUS] = 0.0f;
        } else if (knowhere::IsMetricType(metric, knowhere::metric::IP)) {
            json[knowhere::meta::RANGE_FILTER] = 1000000.0f;
            json[knowhere::meta::RADIUS] = 0.0f;
        }
        return json;
    };

    const auto train_ds = GenDataSet(nb, dim);
    const auto query_ds = GenDataSet(nq, dim);

    const knowhere::Json conf = {
        {knowhere::meta::METRIC_TYPE, metric},
        {knowhere::meta::TOPK, topk},
    };
    auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, conf, nullptr);

    SECTION("Test Search") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IDMAP, flat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT, ivfflat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ8, ivfsq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFPQ, ivfpq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfsqcc_code_size_4_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfsqcc_code_size_6_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfsqcc_code_size_8_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfsqcc_code_size_16_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen2),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_gen),
        }));
        knowhere::BinarySet bs;
        // build process
        {
            auto idx_expected = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version);
            if (name == knowhere::IndexEnum::INDEX_FAISS_SCANN) {
                // need to check cpu model for scann
                if (!faiss::support_pq_fast_scan) {
                    REQUIRE(idx_expected.error() == knowhere::Status::invalid_index_error);
                    return;
                }
            }
            auto idx = idx_expected.value();
            auto cfg_json = gen().dump();
            CAPTURE(name, cfg_json);
            knowhere::Json json = knowhere::Json::parse(cfg_json);
            REQUIRE(idx.Type() == name);
            REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
            REQUIRE(idx.Size() > 0);
            REQUIRE(idx.Count() == nb);

            REQUIRE(idx.Serialize(bs) == knowhere::Status::success);
        }
        // search process
        auto load_with_mmap = GENERATE(as<bool>{}, true, false);
        {
            auto idx_expected = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version);
            auto idx = idx_expected.value();
            auto cfg_json = gen().dump();
            CAPTURE(name, cfg_json);
            knowhere::Json json = knowhere::Json::parse(cfg_json);
            // TODO: qianya(DeserializeFromFile need raw data path. Next pr will remove raw data in ivf sq cc index, and
            // use a knowhere struct to maintain raw data)
            if (load_with_mmap && knowhere::KnowhereCheck::SupportMmapIndexTypeCheck(name) &&
                name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC) {
                auto binary = bs.GetByName(idx.Type());
                auto data = binary->data.get();
                auto size = binary->size;
                std::remove(kMmapIndexPath);
                std::ofstream out(kMmapIndexPath, std::ios::binary);
                out.write((const char*)data, size);
                out.close();
                json["enable_mmap"] = true;
                REQUIRE(idx.DeserializeFromFile(kMmapIndexPath, json) == knowhere::Status::success);
            } else {
                REQUIRE(idx.Deserialize(std::move(bs), json) == knowhere::Status::success);
            }

            // TODO: qianya (IVFSQ_CC deserialize casted from the IVFSQ directly, which will cause the hasRawData
            // reference to an uncertain address)
            if (name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC) {
                REQUIRE(idx.HasRawData(json[knowhere::meta::METRIC_TYPE]) ==
                        knowhere::IndexStaticFaced<knowhere::fp32>::HasRawData(name, version, json));
            }

            auto results = idx.Search(query_ds, json, nullptr);
            REQUIRE(results.has_value());
            float recall = GetKNNRecall(*gt.value(), *results.value());
            bool scann_without_raw_data =
                (name == knowhere::IndexEnum::INDEX_FAISS_SCANN && scann_gen2().dump() == cfg_json);
            if (name != knowhere::IndexEnum::INDEX_FAISS_IVFPQ && !scann_without_raw_data) {
                REQUIRE(recall > kKnnRecallThreshold);
            }

            if (metric == knowhere::metric::COSINE) {
                if (name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ8 && name != knowhere::IndexEnum::INDEX_FAISS_IVFPQ &&
                    name != knowhere::IndexEnum::INDEX_HNSW_SQ && name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC &&
                    !scann_without_raw_data) {
                    REQUIRE(CheckDistanceInScope(*results.value(), topk, -1.00001, 1.00001));
                }
            }
        }
        if (load_with_mmap && knowhere::KnowhereCheck::SupportMmapIndexTypeCheck(name) &&
            name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC) {
            std::remove(kMmapIndexPath);
        }
    }

    SECTION("Test Range Search") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IDMAP, flat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT, ivfflat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ8, ivfsq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFPQ, ivfpq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfsq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen2),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_gen),
        }));
        auto idx_expected = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version);
        if (name == knowhere::IndexEnum::INDEX_FAISS_SCANN) {
            // need to check cpu model for scann
            if (!faiss::support_pq_fast_scan) {
                REQUIRE(idx_expected.error() == knowhere::Status::invalid_index_error);
                return;
            }
        }
        auto idx = idx_expected.value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);

        knowhere::BinarySet bs;
        REQUIRE(idx.Serialize(bs) == knowhere::Status::success);
        REQUIRE(idx.Deserialize(std::move(bs), json) == knowhere::Status::success);

        auto results = idx.RangeSearch(query_ds, json, nullptr);
        REQUIRE(results.has_value());
        auto ids = results.value()->GetIds();
        auto lims = results.value()->GetLims();
        bool scann_without_raw_data =
            (name == knowhere::IndexEnum::INDEX_FAISS_SCANN && scann_gen2().dump() == cfg_json);
        if (name != knowhere::IndexEnum::INDEX_FAISS_IVFPQ && name != knowhere::IndexEnum::INDEX_FAISS_SCANN &&
            name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC) {
            for (int i = 0; i < nq; ++i) {
                CHECK(ids[lims[i]] == i);
            }
        }

        if (metric == knowhere::metric::COSINE) {
            if (name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ8 && name != knowhere::IndexEnum::INDEX_FAISS_IVFPQ &&
                name != knowhere::IndexEnum::INDEX_HNSW_SQ && name != knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC &&
                !scann_without_raw_data) {
                REQUIRE(CheckDistanceInScope(*results.value(), -1.00001, 1.00001));
            }
        }
    }

#ifdef KNOWHERE_WITH_CARDINAL
    // currently, only cardinal support iterator_retain_order
    SECTION("TEST Range Search (iterator-based) with ordered iterator") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, ordered_rs_hnsw_gen),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        knowhere::Json json = knowhere::Json::parse(cfg_json);

        CAPTURE(name, json.dump());
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);

        auto results = idx.RangeSearch(query_ds, json, nullptr);
        REQUIRE(results.has_value());
        auto ids = results.value()->GetIds();
        auto lims = results.value()->GetLims();
        auto dists = results.value()->GetDistance();

        // the results count should not be more than range_search_k and top_k.
        for (int i = 0; i < nq; ++i) {
            int64_t size = lims[i + 1] - lims[i];
            REQUIRE(size <= topk);
        }

        // top-k results should be same with first-k of top-2k.
        json[knowhere::meta::RANGE_SEARCH_K] = topk * 2;
        CAPTURE(name, json.dump());
        auto more_results = idx.RangeSearch(query_ds, json, nullptr);
        REQUIRE(more_results.has_value());
        auto more_ids = more_results.value()->GetIds();
        auto more_lims = more_results.value()->GetLims();
        for (int i = 0; i < nq; ++i) {
            int size = lims[i + 1] - lims[i];
            int more_size = more_lims[i + 1] - more_lims[i];
            REQUIRE(more_size >= size);
            for (int j = 0; j < size; ++j) {
                REQUIRE(ids[lims[i] + j] == more_ids[more_lims[i] + j]);
            }
        }
    }
#endif

    SECTION("Test Search with super large topk") {
        using std::make_tuple;
        auto hnsw_gen_ = [base_gen]() {
            knowhere::Json json = base_gen();
            json[knowhere::indexparam::HNSW_M] = 12;
            json[knowhere::indexparam::EFCONSTRUCTION] = 30;
            json[knowhere::meta::TOPK] = GENERATE(as<int64_t>{}, 600);
            return json;
        };
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen_),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);

        auto results = idx.Search(query_ds, json, nullptr);
        auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, json, nullptr);
        float recall = GetKNNRecall(*gt.value(), *results.value());
        REQUIRE(recall > kBruteForceRecallThreshold);
    }

    SECTION("Test Search with IVFFLATCC ensure topk full") {
        using std::make_tuple;
        auto ivfflatcc_gen_ = [base_gen, nb]() {
            knowhere::Json json = base_gen();
            json[knowhere::indexparam::NLIST] = 32;
            json[knowhere::indexparam::NPROBE] = 1;
            json[knowhere::indexparam::SSIZE] = 48;
            json[knowhere::meta::TOPK] = nb;
            return json;
        };
        auto ivfflatcc_gen_no_ensure_topk_ = [ivfflatcc_gen_, nb]() {
            knowhere::Json json = ivfflatcc_gen_();
            json[knowhere::meta::TOPK] = nb / 2;
            json[knowhere::indexparam::ENSURE_TOPK_FULL] = false;
            return json;
        };
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen_),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfflatcc_gen_),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen_no_ensure_topk_),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfflatcc_gen_no_ensure_topk_),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);

        auto results = idx.Search(query_ds, json, nullptr);
        auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, json, nullptr);
        float recall = GetKNNRecall(*gt.value(), *results.value());
        if (ivfflatcc_gen_().dump() == cfg_json) {
            REQUIRE(recall > kBruteForceRecallThreshold);
        } else {
            REQUIRE(recall < kBruteForceRecallThreshold);
        }

        std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
            GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
        const auto bitset_percentages = 0.5f;
        for (const auto& gen_func : gen_bitset_funcs) {
            auto bitset_data = gen_func(nb, bitset_percentages * nb);
            knowhere::BitsetView bitset(bitset_data.data(), nb);
            auto results = idx.Search(query_ds, json, bitset);
            auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, json, bitset);
            float recall = GetKNNRecall(*gt.value(), *results.value());
            if (ivfflatcc_gen_().dump() == cfg_json) {
                REQUIRE(recall > kBruteForceRecallThreshold);
            } else {
                REQUIRE(recall < kBruteForceRecallThreshold);
            }
        }
    }

    SECTION("Test Search with Bitset") {
        using std::make_tuple;
        auto [name, gen, threshold] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>, float>({
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen, hnswlib::kHnswSearchKnnBFFilterThreshold),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_gen, hnswlib::kHnswSearchKnnBFFilterThreshold),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);

        std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
            GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
        const auto bitset_percentages = {0.4f, 0.8f, 0.98f};
        for (const float percentage : bitset_percentages) {
            for (const auto& gen_func : gen_bitset_funcs) {
                auto bitset_data = gen_func(nb, percentage * nb);
                knowhere::BitsetView bitset(bitset_data.data(), nb);
                auto results = idx.Search(query_ds, json, bitset);
                auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, json, bitset);
                float recall = GetKNNRecall(*gt.value(), *results.value());
                if (percentage > threshold ||
                    json[knowhere::meta::TOPK] > (1 - percentage) * nb * hnswlib::kHnswSearchBFTopkThreshold) {
                    REQUIRE(recall > kBruteForceRecallThreshold);
                } else {
                    REQUIRE(recall > kKnnRecallThreshold);
                }
            }
        }
    }

    SECTION("Test Serialize/Deserialize") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IDMAP, flat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT, ivfflat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ8, ivfsq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFPQ, ivfpq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivfsq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen2),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_gen),
        }));

        auto idx_expected = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version);
        if (name == knowhere::IndexEnum::INDEX_FAISS_SCANN) {
            // need to check cpu model for scann
            if (!faiss::support_pq_fast_scan) {
                REQUIRE(idx_expected.error() == knowhere::Status::invalid_index_error);
                return;
            }
        }
        auto idx = idx_expected.value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        knowhere::BinarySet bs;
        idx.Serialize(bs);

        auto idx_ = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        idx_.Deserialize(std::move(bs));
        auto results = idx_.Search(query_ds, json, nullptr);
        REQUIRE(results.has_value());
    }

    SECTION("Test IVFPQ with invalid params") {
        auto idx = knowhere::IndexFactory::Instance()
                       .Create<knowhere::fp32>(knowhere::IndexEnum::INDEX_FAISS_IVFPQ, version)
                       .value();
        uint32_t nb = 1000;
        uint32_t dim = 128;
        auto ivf_pq_gen = [=]() {
            knowhere::Json json;
            json[knowhere::meta::DIM] = dim;
            json[knowhere::meta::METRIC_TYPE] = knowhere::metric::L2;
            json[knowhere::meta::TOPK] = 10;
            json[knowhere::indexparam::M] = 15;
            json[knowhere::indexparam::NLIST] = 128;
            json[knowhere::indexparam::NBITS] = 8;
            return json;
        };
        auto train_ds = GenDataSet(nb, dim);
        auto res = idx.Build(train_ds, ivf_pq_gen());
        REQUIRE(res == knowhere::Status::invalid_args);
    }

    SECTION("Test IVFPQ with invalid params") {
        auto idx = knowhere::IndexFactory::Instance()
                       .Create<knowhere::fp32>(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, version)
                       .value();
        uint32_t nb = 1000;
        uint32_t dim = 128;
        auto ivf_pq_gen = [=]() {
            knowhere::Json json;
            json[knowhere::meta::DIM] = dim;
            json[knowhere::meta::METRIC_TYPE] = knowhere::metric::L2;
            json[knowhere::meta::TOPK] = 10;
            json[knowhere::indexparam::NLIST] = 128;
            json[knowhere::indexparam::CODE_SIZE] = 7;
            return json;
        };
        auto train_ds = GenDataSet(nb, dim);
        auto res = idx.Build(train_ds, ivf_pq_gen());
        REQUIRE(res == knowhere::Status::invalid_value_in_json);
    }
}

TEST_CASE("Test Mem Index With Binary Vector", "[float metrics]") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;
    const int64_t dim = 1024;

    auto metric = GENERATE(as<std::string>{}, knowhere::metric::HAMMING, knowhere::metric::JACCARD);
    auto topk = GENERATE(as<int64_t>{}, 5, 120);
    auto version = GenTestVersionList();
    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        json[knowhere::meta::RADIUS] = knowhere::IsMetricType(metric, knowhere::metric::HAMMING) ? 10.0 : 0.1;
        json[knowhere::meta::RANGE_FILTER] = 0.0;
        return json;
    };

    auto flat_gen = base_gen;
    auto ivfflat_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NLIST] = 16;
        json[knowhere::indexparam::NPROBE] = 14;
        return json;
    };

    auto hnsw_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::HNSW_M] = 128;
        json[knowhere::indexparam::EFCONSTRUCTION] = 200;
        json[knowhere::indexparam::EF] = 200;
        return json;
    };

    const auto train_ds = GenBinDataSet(nb, dim);
    const auto query_ds = GenBinDataSet(nq, dim);
    const knowhere::Json conf = {
        {knowhere::meta::METRIC_TYPE, metric},
        {knowhere::meta::TOPK, topk},
    };

    auto gt = knowhere::BruteForce::Search<knowhere::bin1>(train_ds, query_ds, conf, nullptr);
    SECTION("Test Search") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP, flat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IVFFLAT, ivfflat_gen),
#ifdef KNOWHERE_WITH_CARDINAL
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
#endif
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::bin1>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        REQUIRE(idx.Size() > 0);
        REQUIRE(idx.Count() == nb);
        auto results = idx.Search(query_ds, json, nullptr);
        REQUIRE(results.has_value());
        float recall = GetKNNRecall(*gt.value(), *results.value());
        REQUIRE(recall > kKnnRecallThreshold);
    }

    SECTION("Test Range Search") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP, flat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IVFFLAT, ivfflat_gen),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::bin1>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        auto results = idx.RangeSearch(query_ds, json, nullptr);
        REQUIRE(results.has_value());
        auto ids = results.value()->GetIds();
        auto lims = results.value()->GetLims();
        for (int i = 0; i < nq; ++i) {
            CHECK(ids[lims[i]] == i);
        }
    }

    SECTION("Test Serialize/Deserialize") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP, flat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IVFFLAT, ivfflat_gen),
        }));

        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::bin1>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        knowhere::BinarySet bs;
        idx.Serialize(bs);

        auto idx_ = knowhere::IndexFactory::Instance().Create<knowhere::bin1>(name, version).value();
        idx_.Deserialize(std::move(bs));
        auto results = idx_.Search(query_ds, json, nullptr);
        REQUIRE(results.has_value());
    }
}

// this is a special case that once triggered a problem in clustering.cpp
TEST_CASE("Test Mem Index With Binary Vector", "[float metrics][special case 1]") {
    using Catch::Approx;

    const int64_t nb = 10, nq = 1;
    const int64_t dim = 16;

    auto metric = GENERATE(as<std::string>{}, knowhere::metric::JACCARD);
    auto topk = GENERATE(as<int64_t>{}, 1, 1);
    auto version = GenTestVersionList();
    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        json[knowhere::meta::RADIUS] = knowhere::IsMetricType(metric, knowhere::metric::HAMMING) ? 10.0 : 0.1;
        json[knowhere::meta::RANGE_FILTER] = 0.0;
        return json;
    };

    auto flat_gen = base_gen;
    auto ivfflat_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NLIST] = 16;
        json[knowhere::indexparam::NPROBE] = 1;
        return json;
    };

    const auto train_ds = GenBinDataSet(nb, dim);
    const auto query_ds = GenBinDataSet(nq, dim);
    const knowhere::Json conf = {
        {knowhere::meta::METRIC_TYPE, metric},
        {knowhere::meta::TOPK, topk},
    };

    auto gt = knowhere::BruteForce::Search<knowhere::bin1>(train_ds, query_ds, conf, nullptr);
    SECTION("Test Search") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IVFFLAT, ivfflat_gen),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::bin1>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        REQUIRE(idx.Size() > 0);
        REQUIRE(idx.Count() == nb);
        auto results = idx.Search(query_ds, json, nullptr);
        REQUIRE(results.has_value());
        float recall = GetKNNRecall(*gt.value(), *results.value());
        REQUIRE(recall > kKnnRecallThreshold);
    }
}

TEST_CASE("Test Mem Index With Binary Vector", "[bool metrics]") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;

    auto dim = GENERATE(as<int64_t>{}, 8, 16, 32, 64, 128, 256, 512, 160);
    auto version = GenTestVersionList();
    auto metric = GENERATE(as<std::string>{}, knowhere::metric::SUPERSTRUCTURE, knowhere::metric::SUBSTRUCTURE);
    auto topk = GENERATE(as<int64_t>{}, 5, 100);

    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        return json;
    };

    auto flat_gen = base_gen;
    auto ivfflat_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NLIST] = 16;
        json[knowhere::indexparam::NPROBE] = 8;
        return json;
    };

    auto GenTestDataSet = [](int rows, int dim) {
        std::mt19937 rng(42);
        std::uniform_int_distribution<> distrib(0.0, 100.0);
        int uint8_num = dim / 8;
        uint8_t* ts = new uint8_t[rows * uint8_num];
        for (int i = 0; i < rows; ++i) {
            auto v = (uint8_t)distrib(rng);
            for (int j = 0; j < uint8_num; ++j) {
                ts[i * uint8_num + j] = v;
            }
        }
        auto ds = knowhere::GenDataSet(rows, dim, ts);
        ds->SetIsOwner(true);
        return ds;
    };
    const auto train_ds = GenTestDataSet(nb, dim);
    const auto query_ds = GenTestDataSet(nq, dim);

    SECTION("Test Search") {
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            std::make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP, flat_gen),
            std::make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IVFFLAT, ivfflat_gen),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::bin1>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        auto res = idx.Build(train_ds, json);
        if (name == knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP) {
            REQUIRE(res == knowhere::Status::success);
        } else {
            REQUIRE(res == knowhere::Status::invalid_metric_type);
            return;
        }
        auto results = idx.Search(query_ds, json, nullptr);
        REQUIRE(results.has_value());
        auto ids = results.value()->GetIds();

        auto code_size = dim / 8;
        for (int64_t i = 0; i < nq; i++) {
            const uint8_t* query_vector = (const uint8_t*)query_ds->GetTensor() + i * code_size;
            // filter out -1 when the result num less than topk
            int64_t real_topk = 0;
            for (; real_topk < topk; real_topk++) {
                if (ids[i * topk + real_topk] < 0)
                    break;
            }
            std::vector<int64_t> ids_v(ids + i * topk, ids + i * topk + real_topk);
            auto ds = GenIdsDataSet(real_topk, ids_v);
            auto gv_res = idx.GetVectorByIds(ds);
            REQUIRE(gv_res.has_value());
            for (int64_t j = 0; j < real_topk; j++) {
                const uint8_t* res_vector = (const uint8_t*)gv_res.value()->GetTensor() + j * code_size;
                if (metric == knowhere::metric::SUPERSTRUCTURE) {
                    REQUIRE(faiss::is_subset(res_vector, query_vector, code_size));
                } else {
                    REQUIRE(faiss::is_subset(query_vector, res_vector, code_size));
                }
            }
        }
    }

#if 0
    SECTION("Test Range Search") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP, flat_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_BIN_IVFFLAT, ivfflat_gen),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        auto res = idx.Build(train_ds, json);
        if (name == knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP) {
            REQUIRE(res == knowhere::Status::success);
        } else {
            REQUIRE(res == knowhere::Status::faiss_inner_error);
            return;
        }
        auto results = idx.RangeSearch(query_ds, json, nullptr);
        REQUIRE(results.error() == knowhere::Status::faiss_inner_error);
    }
#endif
}
