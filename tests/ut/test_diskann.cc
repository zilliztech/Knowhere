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

#include <string>

#include "catch2/catch_approx.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "index/diskann/diskann_config.h"
#include "knowhere/comp/brute_force.h"
#include "knowhere/comp/knowhere_check.h"
#include "knowhere/comp/local_file_manager.h"
#include "knowhere/expected.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/utils.h"
#include "utils.h"
#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
error "Missing the <filesystem> header."
#endif
#include <fstream>

namespace {
std::string kDir = fs::current_path().string() + "/diskann_test";
std::string kRawDataPath = kDir + "/raw_data";
std::string kL2IndexDir = kDir + "/l2_index";
std::string kIPIndexDir = kDir + "/ip_index";
std::string kCOSINEIndexDir = kDir + "/cosine_index";
std::string kL2IndexPrefix = kL2IndexDir + "/l2";
std::string kIPIndexPrefix = kIPIndexDir + "/ip";
std::string kCOSINEIndexPrefix = kCOSINEIndexDir + "/cosine";

constexpr uint32_t kNumRows = 1000;
constexpr uint32_t kNumQueries = 10;
constexpr uint32_t kDim = 128;
constexpr uint32_t kLargeDim = 1536;
constexpr uint32_t kK = 10;
constexpr float kKnnRecall = 0.9;
constexpr float kL2RangeAp = 0.9;
constexpr float kIpRangeAp = 0.9;
constexpr float kCosineRangeAp = 0.9;

template <typename DataType>
void
WriteRawDataToDisk(const std::string data_path, const DataType* raw_data, const uint32_t num, const uint32_t dim) {
    std::ofstream writer(data_path.c_str(), std::ios::binary);
    writer.write((char*)&num, sizeof(uint32_t));
    writer.write((char*)&dim, sizeof(uint32_t));
    writer.write((char*)raw_data, sizeof(DataType) * num * dim);
    writer.close();
}

}  // namespace
TEST_CASE("Valid diskann build params test", "[diskann]") {
    int rows_num = 1000000;
    auto version = GenTestVersionList();

    auto ratio = GENERATE(as<float>{}, 0.01, 0.1, 0.125);

    float pq_code_budget_gb = sizeof(float) * kDim * rows_num * 0.125 / (1024 * 1024 * 1024);
    float search_cache_budget_gb = sizeof(float) * kDim * rows_num * 0.05 / (1024 * 1024 * 1024);

    auto test_gen = [&]() {
        knowhere::Json json;
        json["dim"] = kDim;
        json["metric_type"] = "L2";
        json["k"] = 100;
        json["index_prefix"] = kL2IndexPrefix;
        json["data_path"] = kRawDataPath;
        json["max_degree"] = 24;
        json["search_list_size"] = 64;
        json["vec_field_size_gb"] = 1.0;
        json["pq_code_budget_gb_ratio"] = ratio;
        json["pq_code_budget_gb"] = pq_code_budget_gb;
        json["build_dram_budget_gb"] = 32.0;
        json["search_cache_budget_gb_ratio"] = ratio;
        json["search_cache_budget_gb"] = search_cache_budget_gb;
        json["beamwidth"] = 8;
        json["min_k"] = 10;
        json["max_k"] = 8000;
        return json;
    };

    SECTION("Dynamic param check") {
        knowhere::Json test_json = test_gen();

        auto cfg = knowhere::IndexStaticFaced<float>::CreateConfig(knowhere::IndexEnum::INDEX_DISKANN, version);
        knowhere::Json json_(test_json);
        std::string msg;
        auto res = knowhere::Config::FormatAndCheck(*cfg, json_, &msg);
        REQUIRE(res == knowhere::Status::success);
        res = knowhere::Config::Load(*cfg, json_, knowhere::PARAM_TYPE::TRAIN, &msg);
        REQUIRE(res == knowhere::Status::success);

        knowhere::DiskANNConfig diskCfg = static_cast<const knowhere::DiskANNConfig&>(*cfg);
        REQUIRE(diskCfg.pq_code_budget_gb == std::max(pq_code_budget_gb, 1.0f * ratio));
        REQUIRE(diskCfg.search_cache_budget_gb == std::max(search_cache_budget_gb, 1.0f * ratio));
    }
}

TEST_CASE("Invalid diskann params test", "[diskann]") {
    fs::remove_all(kDir);
    fs::remove(kDir);
    REQUIRE_NOTHROW(fs::create_directory(kDir));
    REQUIRE_NOTHROW(fs::create_directory(kL2IndexDir));
    REQUIRE_NOTHROW(fs::create_directory(kIPIndexDir));
    int rows_num = 10;
    auto version = GenTestVersionList();
    auto test_gen = [rows_num]() {
        knowhere::Json json;
        json["dim"] = kDim;
        json["metric_type"] = "L2";
        json["k"] = 100;
        json["index_prefix"] = kL2IndexPrefix;
        json["data_path"] = kRawDataPath;
        json["max_degree"] = 24;
        json["search_list_size"] = 64;
        json["pq_code_budget_gb"] = sizeof(float) * kDim * rows_num * 0.125 / (1024 * 1024 * 1024);
        json["build_dram_budget_gb"] = 32.0;
        json["search_cache_budget_gb"] = sizeof(float) * kDim * rows_num * 0.05 / (1024 * 1024 * 1024);
        json["beamwidth"] = 8;
        json["min_k"] = 10;
        json["max_k"] = 8000;
        return json;
    };
    std::shared_ptr<knowhere::FileManager> file_manager = std::make_shared<knowhere::LocalFileManager>();
    auto diskann_index_pack = knowhere::Pack(file_manager);
    auto base_ds = GenDataSet(rows_num, kDim, 30);
    auto base_ptr = static_cast<const float*>(base_ds->GetTensor());
    WriteRawDataToDisk<float>(kRawDataPath, base_ptr, rows_num, kDim);
    // build process
    SECTION("Invalid build params test") {
        knowhere::DataSetPtr ds_ptr = nullptr;
        auto diskann =
            knowhere::IndexFactory::Instance().Create<knowhere::fp32>("DISKANN", version, diskann_index_pack).value();
        knowhere::Json test_json;
        knowhere::Status test_stat;
        // invalid metric type
        test_json = test_gen();
        test_json["metric_type"] = knowhere::metric::JACCARD;
        test_stat = diskann.Build(ds_ptr, test_json);
        REQUIRE(test_stat == knowhere::Status::invalid_metric_type);
        // raw data path not exist
        test_json = test_gen();
        test_json["data_path"] = kL2IndexPrefix + ".temp";
        test_stat = diskann.Build(ds_ptr, test_json);
        REQUIRE(test_stat == knowhere::Status::disk_file_error);
    }

    SECTION("Invalid search params test") {
        knowhere::DataSetPtr ds_ptr = nullptr;
        auto binarySet = knowhere::BinarySet();
        auto diskann =
            knowhere::IndexFactory::Instance().Create<knowhere::fp32>("DISKANN", version, diskann_index_pack).value();
        diskann.Build(ds_ptr, test_gen());
        diskann.Serialize(binarySet);
        diskann.Deserialize(std::move(binarySet), test_gen());

        knowhere::Json test_json;
        auto query_ds = GenDataSet(kNumQueries, kDim, 42);

#ifndef KNOWHERE_WITH_CARDINAL
        // search list size < topk
        {
            test_json = test_gen();
            test_json["search_list_size"] = 1;
            auto res = diskann.Search(query_ds, test_json, nullptr);
            REQUIRE_FALSE(res.has_value());
            REQUIRE(res.error() == knowhere::Status::out_of_range_in_json);
        }
#endif
    }
    fs::remove_all(kDir);
    fs::remove(kDir);
}

template <typename DataType>
inline void
base_search() {
    fs::remove_all(kDir);
    fs::remove(kDir);
    REQUIRE_NOTHROW(fs::create_directory(kDir));
    REQUIRE_NOTHROW(fs::create_directory(kL2IndexDir));
    REQUIRE_NOTHROW(fs::create_directory(kIPIndexDir));
    REQUIRE_NOTHROW(fs::create_directory(kCOSINEIndexDir));

    auto metric_str = GENERATE(as<std::string>{}, knowhere::metric::L2, knowhere::metric::IP, knowhere::metric::COSINE);
    auto version = GenTestVersionList();

    std::unordered_map<knowhere::MetricType, std::string> metric_dir_map = {
        {knowhere::metric::L2, kL2IndexPrefix},
        {knowhere::metric::IP, kIPIndexPrefix},
        {knowhere::metric::COSINE, kCOSINEIndexPrefix},
    };
    std::unordered_map<knowhere::MetricType, float> metric_range_ap_map = {
        {knowhere::metric::L2, kL2RangeAp},
        {knowhere::metric::IP, kIpRangeAp},
        {knowhere::metric::COSINE, kCosineRangeAp},
    };

    auto base_gen = [&metric_str]() {
        knowhere::Json json;
        json["dim"] = kDim;
        json["metric_type"] = metric_str;
        json["k"] = kK;
        if (metric_str == knowhere::metric::L2) {
            json["radius"] = CFG_FLOAT::value_type(200000);
            json["range_filter"] = CFG_FLOAT::value_type(0);
        } else if (metric_str == knowhere::metric::IP) {
            json["radius"] = CFG_FLOAT::value_type(350000);
            json["range_filter"] = std::numeric_limits<CFG_FLOAT::value_type>::max();
        } else {
            json["radius"] = 0.75f;
            json["range_filter"] = 1.0f;
        }
        return json;
    };

    auto build_gen = [&base_gen, &metric_str, &metric_dir_map]() {
        knowhere::Json json = base_gen();
        json["index_prefix"] = metric_dir_map[metric_str];
        json["data_path"] = kRawDataPath;
        json["max_degree"] = 56;
        json["search_list_size"] = 128;
        json["pq_code_budget_gb"] = sizeof(float) * kDim * kNumRows * 0.125 / (1024 * 1024 * 1024);
        json["search_cache_budget_gb"] = sizeof(float) * kDim * kNumRows * 0.125 / (1024 * 1024 * 1024);
        json["build_dram_budget_gb"] = 32.0;
        return json;
    };

    auto deserialize_gen = [&base_gen, &metric_str, &metric_dir_map]() {
        knowhere::Json json = base_gen();
        json["index_prefix"] = metric_dir_map[metric_str];
        json["search_cache_budget_gb"] = sizeof(float) * kDim * kNumRows * 0.125 / (1024 * 1024 * 1024);
        return json;
    };

    auto knn_search_gen = [&base_gen, &metric_str, &metric_dir_map]() {
        knowhere::Json json = base_gen();
        json["index_prefix"] = metric_dir_map[metric_str];
        json["search_list_size"] = 36;
        json["beamwidth"] = 8;
        return json;
    };

    auto range_search_gen = [&base_gen, &metric_str, &metric_dir_map]() {
        knowhere::Json json = base_gen();
        json["index_prefix"] = metric_dir_map[metric_str];
        json["beamwidth"] = 8;
        return json;
    };

    auto fp32_query_ds = GenDataSet(kNumQueries, kDim, 42);
    knowhere::DataSetPtr knn_gt_ptr = nullptr;
    knowhere::DataSetPtr range_search_gt_ptr = nullptr;
    auto fp32_base_ds = GenDataSet(kNumRows, kDim, 30);

    auto base_ds = knowhere::ConvertToDataTypeIfNeeded<DataType>(fp32_base_ds);
    auto query_ds = knowhere::ConvertToDataTypeIfNeeded<DataType>(fp32_query_ds);

    {
        auto base_ptr = static_cast<const DataType*>(base_ds->GetTensor());
        WriteRawDataToDisk<DataType>(kRawDataPath, base_ptr, kNumRows, kDim);

        // generate the gt of knn search and range search
        auto base_json = base_gen();

        auto result_knn = knowhere::BruteForce::Search<DataType>(base_ds, query_ds, base_json, nullptr);
        knn_gt_ptr = result_knn.value();
        auto result_range = knowhere::BruteForce::RangeSearch<DataType>(base_ds, query_ds, base_json, nullptr);
        range_search_gt_ptr = result_range.value();
    }

    SECTION("Test search and range search") {
        std::shared_ptr<knowhere::FileManager> file_manager = std::make_shared<knowhere::LocalFileManager>();
        auto diskann_index_pack = knowhere::Pack(file_manager);
        knowhere::Json deserialize_json = knowhere::Json::parse(deserialize_gen().dump());
        knowhere::BinarySet binset;

        auto build_json = build_gen().dump();
        knowhere::Json json = knowhere::Json::parse(build_json);
        // build process
        {
            knowhere::DataSetPtr ds_ptr = nullptr;
            auto diskann =
                knowhere::IndexFactory::Instance().Create<DataType>("DISKANN", version, diskann_index_pack).value();
            diskann.Build(ds_ptr, json);
            diskann.Serialize(binset);
        }
        {
            // knn search
            auto diskann =
                knowhere::IndexFactory::Instance().Create<DataType>("DISKANN", version, diskann_index_pack).value();
            diskann.Deserialize(std::move(binset), deserialize_json);
            REQUIRE(diskann.HasRawData(metric_str) ==
                    knowhere::IndexStaticFaced<DataType>::HasRawData("DISKANN", version, json));

            auto knn_search_json = knn_search_gen().dump();
            knowhere::Json knn_json = knowhere::Json::parse(knn_search_json);
            auto res = diskann.Search(query_ds, knn_json, nullptr);
            REQUIRE(res.has_value());
            auto knn_recall = GetKNNRecall(*knn_gt_ptr, *res.value());
            REQUIRE(knn_recall > kKnnRecall);

            // knn search without cache file
            {
                std::string cached_nodes_file_path =
                    std::string(build_gen()["index_prefix"]) + std::string("_cached_nodes.bin");
                if (fs::exists(cached_nodes_file_path)) {
                    fs::remove(cached_nodes_file_path);
                }
                auto diskann_tmp =
                    knowhere::IndexFactory::Instance().Create<DataType>("DISKANN", version, diskann_index_pack).value();
                diskann_tmp.Deserialize(std::move(binset), deserialize_json);
                auto knn_search_json = knn_search_gen().dump();
                knowhere::Json knn_json = knowhere::Json::parse(knn_search_json);
                auto res = diskann_tmp.Search(query_ds, knn_json, nullptr);
                REQUIRE(res.has_value());
                REQUIRE(GetKNNRecall(*knn_gt_ptr, *res.value()) >= kKnnRecall);
            }

            // knn search with bitset
            std::vector<std::function<std::vector<uint8_t>(size_t, size_t)>> gen_bitset_funcs = {
                GenerateBitsetWithFirstTbitsSet, GenerateBitsetWithRandomTbitsSet};
            const auto bitset_percentages = {0.4f, 0.98f};
            const auto bitset_thresholds = {-1.0f, 0.9f};
            for (const float threshold : bitset_thresholds) {
                knn_json["filter_threshold"] = threshold;
                for (const float percentage : bitset_percentages) {
                    for (const auto& gen_func : gen_bitset_funcs) {
                        auto bitset_data = gen_func(kNumRows, percentage * kNumRows);
                        knowhere::BitsetView bitset(bitset_data.data(), kNumRows);
                        auto results = diskann.Search(query_ds, knn_json, bitset);
                        auto gt = knowhere::BruteForce::Search<DataType>(base_ds, query_ds, knn_json, bitset);
                        float recall = GetKNNRecall(*gt.value(), *results.value());
                        if (percentage == 0.98f) {
                            REQUIRE(recall >= 0.9f);
                        } else {
                            REQUIRE(recall >= kKnnRecall);
                        }
                    }
                }
            }

            // range search process
            auto range_search_json = range_search_gen().dump();
            knowhere::Json range_json = knowhere::Json::parse(range_search_json);
            auto range_search_res = diskann.RangeSearch(query_ds, range_json, nullptr);
            REQUIRE(range_search_res.has_value());
            auto ap = GetRangeSearchRecall(*range_search_gt_ptr, *range_search_res.value());
            float standard_ap = metric_range_ap_map[metric_str];
            REQUIRE(ap > standard_ap);
        }
    }
    fs::remove_all(kDir);
    fs::remove(kDir);
}

TEST_CASE("Test DiskANNIndexNode.", "[diskann]") {
    base_search<knowhere::fp32>();
}

// This test case only check L2
TEST_CASE("Test DiskANN GetVectorByIds", "[diskann]") {
    auto version = GenTestVersionList();
    for (const uint32_t dim : {kDim, kLargeDim}) {
        fs::remove_all(kDir);
        fs::remove(kDir);
        REQUIRE_NOTHROW(fs::create_directories(kL2IndexDir));

        auto base_gen = [=] {
            knowhere::Json json;
            json[knowhere::meta::RETRIEVE_FRIENDLY] = true;
            json["dim"] = dim;
            json["metric_type"] = knowhere::metric::L2;
            json["k"] = kK;
            return json;
        };

        auto build_gen = [=]() {
            knowhere::Json json = base_gen();
            json["index_prefix"] = kL2IndexPrefix;
            json["data_path"] = kRawDataPath;
            json["max_degree"] = 5;
            json["search_list_size"] = kK;
            json["pq_code_budget_gb"] = sizeof(float) * dim * kNumRows * 0.03125 / (1024 * 1024 * 1024);
            json["build_dram_budget_gb"] = 32.0;
            return json;
        };

        auto query_ds = GenDataSet(kNumQueries, dim, 42);
        auto base_ds = GenDataSet(kNumRows, dim, 30);
        auto base_ptr = static_cast<const float*>(base_ds->GetTensor());
        WriteRawDataToDisk<float>(kRawDataPath, base_ptr, kNumRows, dim);

        std::shared_ptr<knowhere::FileManager> file_manager = std::make_shared<knowhere::LocalFileManager>();
        auto diskann_index_pack = knowhere::Pack(file_manager);

        knowhere::DataSetPtr ds_ptr = nullptr;
        auto diskann =
            knowhere::IndexFactory::Instance().Create<knowhere::fp32>("DISKANN", version, diskann_index_pack).value();
        auto build_json = build_gen().dump();
        knowhere::Json json = knowhere::Json::parse(build_json);
        diskann.Build(ds_ptr, json);
        knowhere::BinarySet binset;
        diskann.Serialize(binset);
        {
            std::vector<double> cache_sizes = {0, 1.0f * sizeof(float) * dim * kNumRows * 0.125 / (1024 * 1024 * 1024)};
            for (const auto cache_size : cache_sizes) {
                auto deserialize_gen = [&base_gen, cache = cache_size]() {
                    knowhere::Json json = base_gen();
                    json["index_prefix"] = kL2IndexPrefix;
                    json["search_cache_budget_gb"] = cache;
                    return json;
                };
                knowhere::Json deserialize_json = knowhere::Json::parse(deserialize_gen().dump());
                auto index = knowhere::IndexFactory::Instance()
                                 .Create<knowhere::fp32>("DISKANN", version, diskann_index_pack)
                                 .value();
                auto ret = index.Deserialize(std::move(binset), deserialize_json);
                REQUIRE(ret == knowhere::Status::success);

                REQUIRE(diskann.HasRawData(knowhere::metric::L2) ==
                        knowhere::IndexStaticFaced<knowhere::fp32>::HasRawData("DISKANN", version, json));

                std::vector<double> ids_sizes = {1, kNumRows * 0.2, kNumRows * 0.7, kNumRows};
                for (const auto ids_size : ids_sizes) {
                    std::cout << "Testing dim = " << dim << ", cache_size = " << cache_size
                              << ", ids_size = " << ids_size << std::endl;
                    auto ids_ds = GenIdsDataSet(ids_size, ids_size);
                    auto results = index.GetVectorByIds(ids_ds);
                    REQUIRE(results.has_value());
                    auto xb = (float*)base_ds->GetTensor();
                    auto data = (float*)results.value()->GetTensor();
                    for (size_t i = 0; i < ids_size; ++i) {
                        auto id = ids_ds->GetIds()[i];
                        for (size_t j = 0; j < dim; ++j) {
                            REQUIRE(data[i * dim + j] == xb[id * dim + j]);
                        }
                    }
                }
            }
        }
    }
    fs::remove_all(kDir);
    fs::remove(kDir);
}
