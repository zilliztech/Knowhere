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

#include <unordered_set>

#include "catch2/catch_approx.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "faiss/utils/binary_distances.h"
#include "hnswlib/hnswalg.h"
#include "knowhere/bitsetview.h"
#include "knowhere/comp/brute_force.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/comp/knowhere_config.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/log.h"
#include "utils.h"

namespace {
constexpr float kKnnRecallThreshold = 0.8f;

knowhere::DataSetPtr
GetIteratorKNNResult(const std::vector<std::shared_ptr<knowhere::IndexNode::iterator>>& iterators, int k,
                     const knowhere::BitsetView* bitset = nullptr) {
    int nq = iterators.size();
    auto p_id = new int64_t[nq * k];
    auto p_dist = new float[nq * k];
    for (int i = 0; i < nq; ++i) {
        auto& iter = iterators[i];
        for (int j = 0; j < k; ++j) {
            if (iter->HasNext()) {
                auto [id, dist] = iter->Next();
                // if bitset is provided, verify we don't return filtered out points.
                REQUIRE((!bitset || !bitset->test(id)));
                p_id[i * k + j] = id;
                p_dist[i * k + j] = dist;
            } else {
                break;
            }
        }
    }
    return knowhere::GenResultDataSet(nq, k, p_id, p_dist);
}

// BruteForce Iterator should return vectors in the exact same order as BruteForce search.
void
AssertBruteForceIteratorResultCorrect(size_t nb,
                                      const std::vector<std::shared_ptr<knowhere::IndexNode::iterator>>& iterators,
                                      const knowhere::DataSetPtr gt) {
    int nq = iterators.size();
    for (int i = 0; i < nq; ++i) {
        auto& iter = *iterators[i];
        auto gt_ids = gt->GetIds() + i * nb;
        auto gt_dist = gt->GetDistance() + i * nb;

        std::unordered_set<int64_t> ids_set;
        size_t j = 0;
        while (j < nb) {
            if (gt_ids[j] == -1) {
                REQUIRE(!iter.HasNext());
                break;
            }
            auto dis = gt_dist[j];
            ids_set.insert(gt_ids[j]);
            while (j + 1 < nb && gt_dist[j + 1] == dis) {
                ids_set.insert(gt_ids[++j]);
            }
            ++j;
            while (!ids_set.empty()) {
                REQUIRE(iter.HasNext());
                auto [id, dist] = iter.Next();
                REQUIRE(ids_set.find(id) != ids_set.end());
                REQUIRE(dist == dis);
                ids_set.erase(id);
            }
        }
    }
}
}  // namespace

// use kNN search to test the correctness of iterator
TEST_CASE("Test Iterator Mem Index With Float Vector", "[float metrics]") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;
    const int64_t dim = 128;
    auto topk = GENERATE(5, 10, 20);

    auto metric = GENERATE(as<std::string>{}, knowhere::metric::L2, knowhere::metric::COSINE, knowhere::metric::IP);
    auto version = GenTestVersionList();

    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        return json;
    };

    auto hnsw_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::HNSW_M] = 128;
        json[knowhere::indexparam::EFCONSTRUCTION] = 200;
        json[knowhere::indexparam::EF] = 80;
        return json;
    };

    auto hnsw_sq_gen = [hnsw_gen]() {
        knowhere::Json json = hnsw_gen();
        json[knowhere::indexparam::SQ_TYPE] = "SQ8";
        return json;
    };

    auto hnsw_sq_refine_flat_gen = [hnsw_sq_gen]() {
        knowhere::Json json = hnsw_sq_gen();
        json["refine"] = true;
        json["refine_type"] = "FLAT";
        return json;
    };

    auto hnsw_sq_refine_fp16_gen = [hnsw_sq_gen]() {
        knowhere::Json json = hnsw_sq_gen();
        json["refine"] = true;
        json["refine_type"] = "FP16";
        return json;
    };

    auto hnsw_pq_gen = [hnsw_gen]() {
        knowhere::Json json = hnsw_gen();
        json[knowhere::indexparam::M] = dim / 8;
        json[knowhere::indexparam::NBITS] = 6;
        return json;
    };

    auto hnsw_pq_refine_flat_gen = [hnsw_pq_gen]() {
        knowhere::Json json = hnsw_pq_gen();
        json["refine"] = true;
        json["refine_type"] = "FLAT";
        return json;
    };

    auto hnsw_pq_refine_sq8_gen = [hnsw_pq_gen]() {
        knowhere::Json json = hnsw_pq_gen();
        json["refine"] = true;
        json["refine_type"] = "SQ8";
        return json;
    };

    auto hnsw_prq_gen = [hnsw_gen]() {
        knowhere::Json json = hnsw_gen();
        json[knowhere::indexparam::M] = dim / 8;
        json[knowhere::indexparam::PRQ_NUM] = 2;
        json[knowhere::indexparam::NBITS] = 6;
        return json;
    };

    auto ivf_base_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NPROBE] = 16;
        json[knowhere::indexparam::NLIST] = 24;
        return json;
    };

    auto ivfflatcc_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NPROBE] = 16;
        json[knowhere::indexparam::NLIST] = 24;
        json[knowhere::indexparam::SSIZE] = 32;
        return json;
    };

    auto ivf_sq_cc_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NPROBE] = 16;
        json[knowhere::indexparam::NLIST] = 24;
        json[knowhere::indexparam::SSIZE] = 32;
        json[knowhere::indexparam::CODE_SIZE] = 8;
        json[knowhere::indexparam::ENSURE_TOPK_FULL] = false;
        return json;
    };

    auto scann_gen = [ivf_base_gen]() {
        knowhere::Json json = ivf_base_gen();
        json[knowhere::indexparam::NPROBE] = 14;
        json[knowhere::indexparam::REORDER_K] = 200;
        json[knowhere::indexparam::WITH_RAW_DATA] = true;
        return json;
    };

    auto scann_gen2 = [ivf_base_gen]() {
        knowhere::Json json = ivf_base_gen();
        json[knowhere::indexparam::WITH_RAW_DATA] = false;
        return json;
    };

    auto rand = GENERATE(1, 2);

    const auto train_ds = GenDataSet(nb, dim, rand);
    const auto query_ds = GenDataSet(nq, dim, rand + 777);

    // certain unit tests are disabled, because they are way too slow at this moment
    // todo: re-enable later
    SECTION("Test Search using iterator") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT, ivf_base_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ8, ivf_base_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivf_sq_cc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_refine_flat_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_refine_fp16_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_refine_flat_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_refine_sq8_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_PRQ, hnsw_prq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen2),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        REQUIRE(idx.Size() > 0);
        REQUIRE(idx.Count() == nb);

        knowhere::BinarySet bs;
        REQUIRE(idx.Serialize(bs) == knowhere::Status::success);
        REQUIRE(idx.Deserialize(std::move(bs)) == knowhere::Status::success);
        auto its = idx.AnnIterator(query_ds, json, nullptr);
        REQUIRE(its.has_value());

        // compare iterator_search results with normal ann search results
        // the iterator resutls should not be too bad.
        auto iterator_results = GetIteratorKNNResult(its.value(), topk);
        auto search_results = idx.Search(query_ds, json, nullptr);
        REQUIRE(search_results.has_value());
        bool dist_less_better = knowhere::IsMetricType(metric, knowhere::metric::L2);
        float recall = GetKNNRelativeRecall(*search_results.value(), *iterator_results, dist_less_better);
        REQUIRE(recall > kKnnRecallThreshold);
    }

#ifdef KNOWHERE_WITH_CARDINAL
    // currently, only cardinal support iterator_retain_order
    SECTION("Test Search with ordered Iterator") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>(
            {make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen)}));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        // set `retain_iterator_order` to true.
        json[knowhere::meta::RETAIN_ITERATOR_ORDER] = true;

        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        auto its = idx.AnnIterator(query_ds, json, nullptr);
        REQUIRE(its.has_value());

        bool dist_less_better = knowhere::IsMetricType(metric, knowhere::metric::L2);
        auto is_first_closer_or_equal = [&dist_less_better](float a, float b) {
            return dist_less_better ? a <= b : a >= b;
        };

        // next_dist should not be closer.
        for (int i = 0; i < nq; ++i) {
            auto& iter = its.value()[i];
            float last_dist;
            if (iter->HasNext()) {
                auto [id, dist] = iter->Next();
                last_dist = dist;
            } else {
                continue;
            }

            while (iter->HasNext()) {
                auto [id, dist] = iter->Next();
                REQUIRE(is_first_closer_or_equal(last_dist, dist));
                last_dist = dist;
            }
        }
    }
#endif

    // certain unit tests are disabled, because they are way too slow at this moment
    // todo: re-enable later
    SECTION("Test Search with Bitset using iterator") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT, ivf_base_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ8, ivf_base_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivf_sq_cc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_refine_flat_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_refine_fp16_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_refine_flat_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_refine_sq8_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_PRQ, hnsw_prq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen2),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);

        std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
            GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
        const auto bitset_percentages = {0.4f, 0.98f};
        for (const float percentage : bitset_percentages) {
            for (const auto& gen_func : gen_bitset_funcs) {
                auto bitset_data = gen_func(nb, percentage * nb);
                knowhere::BitsetView bitset(bitset_data.data(), nb);
                // Iterator doesn't have a fallback to bruteforce mechanism at high filter rate.
                auto its = idx.AnnIterator(query_ds, json, bitset);
                REQUIRE(its.has_value());

                auto iterator_results = GetIteratorKNNResult(its.value(), topk);
                auto search_results = idx.Search(query_ds, json, bitset);
                REQUIRE(search_results.has_value());
                bool dist_less_better = knowhere::IsMetricType(metric, knowhere::metric::L2);
                float recall = GetKNNRelativeRecall(*search_results.value(), *iterator_results, dist_less_better);
                REQUIRE(recall > kKnnRecallThreshold);
            }
        }
    }

    // certain unit tests are disabled, because they are way too slow at this moment
    // todo: re-enable later
    SECTION("Test Search with Bitset using iterator insufficient results") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT, ivf_base_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ8, ivf_base_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC, ivf_sq_cc_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_refine_flat_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_SQ, hnsw_sq_refine_fp16_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_refine_flat_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_PQ, hnsw_pq_refine_sq8_gen),
            // make_tuple(knowhere::IndexEnum::INDEX_HNSW_PRQ, hnsw_prq_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen),
            make_tuple(knowhere::IndexEnum::INDEX_FAISS_SCANN, scann_gen2),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);

        std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
            GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
        // we want topk but after filtering we have only half of topk points.
        const auto filter_remaining = topk / 2;
        for (const auto& gen_func : gen_bitset_funcs) {
            auto bitset_data = gen_func(nb, nb - filter_remaining);
            knowhere::BitsetView bitset(bitset_data.data(), nb);
            auto its = idx.AnnIterator(query_ds, json, bitset);
            REQUIRE(its.has_value());
            auto iterator_results = GetIteratorKNNResult(its.value(), topk);
            // after get those remaining points, iterator should return false for HasNext.
            for (const auto& it : its.value()) {
                REQUIRE(!it->HasNext());
            }
            auto search_results = idx.Search(query_ds, json, bitset);
            REQUIRE(search_results.has_value());
            bool dist_less_better = knowhere::IsMetricType(metric, knowhere::metric::L2);
            float recall = GetKNNRelativeRecall(*search_results.value(), *iterator_results, dist_less_better);
            REQUIRE(recall > kKnnRecallThreshold);
        }
    }
}

// ivfflatcc iterator should not scan newly added vectors.
TEST_CASE("Test Iterator IVFFlatCC With Newly Insert Vectors", "[float metrics] ") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;
    const int64_t dim = 128;
    const int64_t topk = nb;
    auto nb_new = GENERATE(100, 1000, 4000);

    auto metric = GENERATE(as<std::string>{}, knowhere::metric::L2, knowhere::metric::COSINE, knowhere::metric::IP);
    auto version = GenTestVersionList();

    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        return json;
    };

    auto ivfflatcc_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::NPROBE] = 16;
        json[knowhere::indexparam::NLIST] = 24;
        json[knowhere::indexparam::SSIZE] = 32;
        return json;
    };

    auto rand = GENERATE(1, 3, 5);

    const auto train_ds = GenDataSet(nb, dim, rand);
    const auto train_ds_new = GenDataSet(nb_new, dim, rand);
    const auto query_ds = GenDataSet(nq, dim, rand + 777);

    SECTION("Test Search using iterator with newly inserted vectors") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>(
            {make_tuple(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC, ivfflatcc_gen)}));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json, nb_new);

        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        REQUIRE(idx.Size() > 0);

        REQUIRE(idx.Count() == nb);
        auto its_old = idx.AnnIterator(query_ds, json, nullptr);
        REQUIRE(its_old.has_value());
        auto& iters_old = its_old.value();

        REQUIRE(idx.Add(train_ds_new, json) == knowhere::Status::success);
        REQUIRE(idx.Count() == nb + nb_new);
        auto its_new = idx.AnnIterator(query_ds, json, nullptr);
        REQUIRE(its_new.has_value());
        auto& iters_new = its_new.value();

        // make sure old_iterator will not return newly inserted vectors
        for (int i = 0; i < iters_old.size(); ++i) {
            auto& iter = iters_old[i];
            for (int j = 0; j < nb; ++j) {
                REQUIRE(iter->HasNext());
                auto [id, dist] = iter->Next();
                REQUIRE(id < nb);
            }
        }

        // make sure new_iterator will get sufficient results (nb + nb_new)
        for (int i = 0; i < iters_new.size(); ++i) {
            auto& iter = iters_new[i];
            bool id_larger_than_nb = false;
            for (int j = 0; j < nb + nb_new; ++j) {
                REQUIRE(iter->HasNext());
                auto [id, dist] = iter->Next();
                id_larger_than_nb |= id >= nb;
            }
            REQUIRE(id_larger_than_nb);
        }
    }
}

TEST_CASE("Test Iterator Mem Index With Binary Metrics", "[binary metrics]") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;
    const int64_t dim = 1024;
    const int64_t topk = 5;

    auto metric = GENERATE(as<std::string>{}, knowhere::metric::HAMMING, knowhere::metric::JACCARD);
    auto version = GenTestVersionList();

    auto base_gen = [=]() {
        knowhere::Json json;
        json[knowhere::meta::DIM] = dim;
        json[knowhere::meta::METRIC_TYPE] = metric;
        json[knowhere::meta::TOPK] = topk;
        return json;
    };

    auto hnsw_gen = [base_gen]() {
        knowhere::Json json = base_gen();
        json[knowhere::indexparam::HNSW_M] = 128;
        json[knowhere::indexparam::EFCONSTRUCTION] = 200;
        json[knowhere::indexparam::EF] = 64;
        return json;
    };
    const auto train_ds = GenBinDataSet(nb, dim);
    const auto query_ds = GenBinDataSet(nq, dim);

    const knowhere::Json conf = {
        {knowhere::meta::METRIC_TYPE, metric},
        {knowhere::meta::TOPK, topk},
    };

#ifdef KNOWHERE_WITH_CARDINAL
    auto gt = knowhere::BruteForce::Search<knowhere::bin1>(train_ds, query_ds, conf, nullptr);
    SECTION("Test Search using iterator") {
        using std::make_tuple;
        auto [name, gen] = GENERATE_REF(table<std::string, std::function<knowhere::Json()>>({
            make_tuple(knowhere::IndexEnum::INDEX_HNSW, hnsw_gen),
        }));
        auto idx = knowhere::IndexFactory::Instance().Create<knowhere::bin1>(name, version).value();
        auto cfg_json = gen().dump();
        CAPTURE(name, cfg_json);
        knowhere::Json json = knowhere::Json::parse(cfg_json);
        REQUIRE(idx.Type() == name);
        REQUIRE(idx.Build(train_ds, json) == knowhere::Status::success);
        REQUIRE(idx.Size() > 0);
        REQUIRE(idx.Count() == nb);

        auto its = idx.AnnIterator(query_ds, json, nullptr);
        REQUIRE(its.has_value());

        auto iterator_results = GetIteratorKNNResult(its.value(), topk);
        auto search_results = idx.Search(query_ds, json, nullptr);
        REQUIRE(search_results.has_value());
        bool dist_less_better = knowhere::IsMetricType(metric, knowhere::metric::L2);
        float recall = GetKNNRelativeRecall(*search_results.value(), *iterator_results, dist_less_better);
        REQUIRE(recall > kKnnRecallThreshold);
    }
#endif
}

TEST_CASE("Test Iterator BruteForce With Float Vector", "[float metrics]") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;
    const int64_t dim = 4;

    auto metric = GENERATE(as<std::string>{}, knowhere::metric::L2, knowhere::metric::IP, knowhere::metric::COSINE);
    auto version = GenTestVersionList();

    auto rand = GENERATE(1, 2, 3, 5);

    const auto train_ds = GenDataSet(nb, dim, rand);
    const auto query_ds = GenDataSet(nq, dim, rand + 777);

    const knowhere::Json conf = {
        {knowhere::meta::METRIC_TYPE, metric}, {knowhere::meta::TOPK, nb},  // to return all vectors
    };

    SECTION("Test Iterator BruteForce") {
        auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, conf, nullptr);
        auto iterators = knowhere::BruteForce::AnnIterator<knowhere::fp32>(train_ds, query_ds, conf, nullptr).value();
        AssertBruteForceIteratorResultCorrect(nb, iterators, gt.value());
    }

    SECTION("Test Iterator BruteForce with filtering") {
        std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
            GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
        const auto bitset_percentages = {0.4f, 0.98f};
        for (const float percentage : bitset_percentages) {
            for (const auto& gen_func : gen_bitset_funcs) {
                auto bitset_data = gen_func(nb, percentage * nb);
                knowhere::BitsetView bitset(bitset_data.data(), nb);
                auto iterators =
                    knowhere::BruteForce::AnnIterator<knowhere::fp32>(train_ds, query_ds, conf, bitset).value();
                auto gt = knowhere::BruteForce::Search<knowhere::fp32>(train_ds, query_ds, conf, bitset);
                AssertBruteForceIteratorResultCorrect(nb, iterators, gt.value());
            }
        }
    }
}

TEST_CASE("Test Iterator BruteForce With Sparse Float Vector", "[IP metric]") {
    using Catch::Approx;

    const int64_t nb = 1000, nq = 10;
    const int64_t dim = 4;

    auto metric = GENERATE(knowhere::metric::IP, knowhere::metric::BM25);
    auto version = GenTestVersionList();

    auto rand = GENERATE(1, 2, 3, 5);

    const auto train_ds = GenSparseDataSet(nb, dim, 0.9, rand);
    const auto query_ds = GenSparseDataSet(nq, dim, 0.9, rand + 777);

    const knowhere::Json conf = {
        {knowhere::meta::METRIC_TYPE, metric}, {knowhere::meta::TOPK, nb},  // to return all vectors
        {knowhere::meta::BM25_K1, 1.2},        {knowhere::meta::BM25_B, 0.75}, {knowhere::meta::BM25_AVGDL, 100},
    };

    SECTION("Test Iterator BruteForce") {
        auto gt = knowhere::BruteForce::SearchSparse(train_ds, query_ds, conf, nullptr);
        auto iterators =
            knowhere::BruteForce::AnnIterator<knowhere::sparse::SparseRow<float>>(train_ds, query_ds, conf, nullptr)
                .value();
        AssertBruteForceIteratorResultCorrect(nb, iterators, gt.value());
    }

    SECTION("Test Iterator BruteForce with filtering") {
        std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
            GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
        const auto bitset_percentages = {0.4f, 0.98f};
        for (const float percentage : bitset_percentages) {
            for (const auto& gen_func : gen_bitset_funcs) {
                auto bitset_data = gen_func(nb, percentage * nb);
                knowhere::BitsetView bitset(bitset_data.data(), nb);
                auto iterators = knowhere::BruteForce::AnnIterator<knowhere::sparse::SparseRow<float>>(
                                     train_ds, query_ds, conf, bitset)
                                     .value();
                auto gt = knowhere::BruteForce::SearchSparse(train_ds, query_ds, conf, bitset);
                AssertBruteForceIteratorResultCorrect(nb, iterators, gt.value());
            }
        }
    }
}
