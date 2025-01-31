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

#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "index/flat/flat_config.h"
#include "index/hnsw/hnsw_config.h"
#include "index/ivf/ivf_config.h"
#include "index/sparse/sparse_inverted_index_config.h"
#include "knowhere/config.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/version.h"
#ifdef KNOWHERE_WITH_DISKANN
#include "index/diskann/diskann_config.h"
#endif
#ifdef KNOWHERE_WITH_CUVS
#include "index/gpu_raft/gpu_raft_cagra_config.h"
#endif

void
checkBuildConfig(knowhere::IndexType indexType, knowhere::Json& json) {
    std::string msg;
    if (knowhere::IndexFactory::Instance().FeatureCheck(indexType, knowhere::feature::BINARY)) {
        CHECK(knowhere::IndexStaticFaced<knowhere::bin1>::ConfigCheck(
                  indexType, knowhere::Version::GetCurrentVersion().VersionNumber(), json, msg) ==
              knowhere::Status::success);
        CHECK(msg.empty());
    }
    if (knowhere::IndexFactory::Instance().FeatureCheck(indexType, knowhere::feature::FLOAT32)) {
        CHECK(knowhere::IndexStaticFaced<float>::ConfigCheck(indexType,
                                                             knowhere::Version::GetCurrentVersion().VersionNumber(),
                                                             json, msg) == knowhere::Status::success);
        CHECK(msg.empty());
    }
    if (knowhere::IndexFactory::Instance().FeatureCheck(indexType, knowhere::feature::BF16)) {
        CHECK(knowhere::IndexStaticFaced<knowhere::bf16>::ConfigCheck(
                  indexType, knowhere::Version::GetCurrentVersion().VersionNumber(), json, msg) ==
              knowhere::Status::success);
        CHECK(msg.empty());
    }
    if (knowhere::IndexFactory::Instance().FeatureCheck(indexType, knowhere::feature::FP16)) {
        CHECK(knowhere::IndexStaticFaced<knowhere::fp16>::ConfigCheck(
                  indexType, knowhere::Version::GetCurrentVersion().VersionNumber(), json, msg) ==
              knowhere::Status::success);
        CHECK(msg.empty());
    }
    if (knowhere::IndexFactory::Instance().FeatureCheck(indexType, knowhere::feature::SPARSE_FLOAT32)) {
        CHECK(knowhere::IndexStaticFaced<float>::ConfigCheck(indexType,
                                                             knowhere::Version::GetCurrentVersion().VersionNumber(),
                                                             json, msg) == knowhere::Status::success);
        CHECK(msg.empty());
    }
#ifndef KNOWHERE_WITH_CARDINAL
    if (knowhere::IndexFactory::Instance().FeatureCheck(indexType, knowhere::feature::INT8)) {
        CHECK(knowhere::IndexStaticFaced<knowhere::int8>::ConfigCheck(
                  indexType, knowhere::Version::GetCurrentVersion().VersionNumber(), json, msg) ==
              knowhere::Status::success);
        CHECK(msg.empty());
    }
#endif
}

TEST_CASE("Test config json parse", "[config]") {
    knowhere::Status s;
    std::string err_msg;
    SECTION("check invalid json keys") {
        auto invalid_json_str = GENERATE(as<std::string>{},
                                         R"({
                "metric_type": "L2",
                "invalid_key": 100
            })",
                                         R"({
                "collection_id": 100,
                "segments_id": 101
            })",
                                         R"({
                "": 0
            })",
                                         R"({
                " ": 0
            })",
                                         R"({
                "topk": 100.1
            })",
                                         R"({
                "metric": "L2"
            })",
                                         R"({
                "12-s": 19878
            })");
        knowhere::BaseConfig test_config;
        knowhere::Json test_json = knowhere::Json::parse(invalid_json_str);
        s = knowhere::Config::FormatAndCheck(test_config, test_json);
        CHECK(s == knowhere::Status::success);
    }

    SECTION("check int64 json values") {
        auto long_int_json_str = GENERATE(as<std::string>{},
                                          R"({
                "dim": 10000000000
            })",
                                          R"({
                "dim": "10000000000"
            })");
        knowhere::BaseConfig test_config;
        knowhere::Json test_json = knowhere::Json::parse(long_int_json_str);
        s = knowhere::Config::FormatAndCheck(test_config, test_json);
        CHECK(s == knowhere::Status::success);
        s = knowhere::Config::Load(test_config, test_json, knowhere::TRAIN);
        CHECK(s == knowhere::Status::success);
        CHECK(test_config.dim.value() == 10000000000L);
    }

    SECTION("check range data values") {
        auto sparse_valid = GENERATE(as<std::string>{},
                                     R"({
                "drop_ratio_build": 0.0
            })");
        knowhere::BaseConfig test_config;
        knowhere::Json test_json = knowhere::Json::parse(sparse_valid);
        s = knowhere::Config::FormatAndCheck(test_config, test_json);
        CHECK(s == knowhere::Status::success);
        s = knowhere::Config::Load(test_config, test_json, knowhere::TRAIN);
        CHECK(s == knowhere::Status::success);

        auto sparse_invalid = GENERATE(as<std::string>{},
                                       R"({
                "drop_ratio_build": 1.0
            })");

        knowhere::SparseInvertedIndexConfig test_invalid_config;
        knowhere::Json test_invalid_json = knowhere::Json::parse(sparse_invalid);
        s = knowhere::Config::FormatAndCheck(test_invalid_config, test_invalid_json);
        CHECK(s == knowhere::Status::success);
        s = knowhere::Config::Load(test_invalid_config, test_invalid_json, knowhere::TRAIN);
        CHECK(s == knowhere::Status::out_of_range_in_json);
    }

    SECTION("check invalid json values") {
        auto invalid_json_str = GENERATE(as<std::string>{},
                                         R"({
                "k": "100.12"
            })");
        knowhere::BaseConfig test_config;
        knowhere::Json test_json = knowhere::Json::parse(invalid_json_str);
        s = knowhere::Config::FormatAndCheck(test_config, test_json);
        CHECK(s == knowhere::Status::invalid_value_in_json);
    }

    SECTION("Check the json for the specific index") {
        knowhere::Json large_build_json = knowhere::Json::parse(R"({
            "beamwidth_ratio":"4.000000",
            "build_dram_budget_gb":4.38,
            "collection_id":"438538303581716485",
            "data_path":"temp",
            "dim":128,
            "disk_pq_dims":0,
            "field_id":"102",
            "index_build_id":"438538303582116508",
            "index_id":"0",
            "index_prefix":"temp",
            "index_type":"DISKANN",
            "index_version":"1",
            "max_degree":56,
            "metric_type":"L2",
            "num_build_thread":2,
            "num_build_thread_ratio":"1.000000",
            "num_load_thread":8,
            "num_load_thread_ratio":"8.000000",
            "partition_id":"438538303581716486",
            "pq_code_budget_gb":0.011920999735593796,
            "pq_code_budget_gb_ratio":"0.125000",
            "search_cache_budget_gb_ratio":"0.100000",
            "search_list_size":100,
            "segment_id":"438538303581916493"
        })");
        knowhere::HnswConfig hnsw_config;
        s = knowhere::Config::FormatAndCheck(hnsw_config, large_build_json);

        checkBuildConfig(knowhere::IndexEnum::INDEX_HNSW, large_build_json);

        CHECK(s == knowhere::Status::success);
#ifdef KNOWHERE_WITH_DISKANN
        knowhere::DiskANNConfig diskann_config;

        checkBuildConfig(knowhere::IndexEnum::INDEX_DISKANN, large_build_json);

        s = knowhere::Config::FormatAndCheck(diskann_config, large_build_json);
        CHECK(s == knowhere::Status::success);
#endif
    }

    SECTION("check materialized view config") {
        knowhere::Json json = knowhere::Json::parse(R"({
            "opt_fields_path": "/tmp/test",
            "materialized_view_search_info": {
                "field_id_to_touched_categories_cnt": [[1,2]],
                "is_pure_and": false,
                "has_not": true
            }
        })");
        knowhere::Status s;
        knowhere::BaseConfig train_cfg;
        s = knowhere::Config::Load(train_cfg, json, knowhere::TRAIN);
        CHECK(s == knowhere::Status::success);
        CHECK(train_cfg.opt_fields_path.value() == "/tmp/test");
        CHECK(train_cfg.materialized_view_search_info.has_value() == false);

        knowhere::BaseConfig search_config;
        s = knowhere::Config::Load(search_config, json, knowhere::SEARCH);
        CHECK(s == knowhere::Status::success);
        CHECK(search_config.opt_fields_path.has_value() == false);
        auto mv = search_config.materialized_view_search_info.value();
        CHECK(mv.field_id_to_touched_categories_cnt.size() == 1);
        CHECK(mv.field_id_to_touched_categories_cnt[1] == 2);
        CHECK(mv.is_pure_and == false);
        CHECK(mv.has_not == true);
    }

    SECTION("check flat index config") {
        knowhere::Json json = knowhere::Json::parse(R"({
            "metric_type": "L2",
            "k": 100
        })");

        checkBuildConfig(knowhere::IndexEnum::INDEX_FAISS_IDMAP, json);
        knowhere::FlatConfig train_cfg;
        s = knowhere::Config::Load(train_cfg, json, knowhere::TRAIN);
        CHECK(s == knowhere::Status::success);
        CHECK(train_cfg.metric_type.value() == "L2");

        knowhere::FlatConfig search_cfg;
        s = knowhere::Config::Load(search_cfg, json, knowhere::SEARCH);
        CHECK(s == knowhere::Status::success);
        CHECK(search_cfg.metric_type.value() == "L2");
        CHECK(search_cfg.k.value() == 100);
    }

    SECTION("check ivf index config") {
        knowhere::Json json = knowhere::Json::parse(R"({
            "metric_type": "L2",
            "k": 100,
            "nlist": 128,
            "nprobe": 16,
            "radius": 1000.0,
            "range_filter": 1.0,
            "trace_visit": true
        })");
        knowhere::IvfFlatConfig train_cfg;
        checkBuildConfig(knowhere::IndexEnum::INDEX_FAISS_IVFFLAT, json);
        s = knowhere::Config::Load(train_cfg, json, knowhere::TRAIN);
        CHECK(s == knowhere::Status::success);
        CHECK(train_cfg.metric_type.value() == "L2");
        CHECK(train_cfg.nlist.value() == 128);

        knowhere::IvfFlatConfig search_cfg;
        s = knowhere::Config::Load(search_cfg, json, knowhere::SEARCH);
        CHECK(s == knowhere::Status::success);
        CHECK(search_cfg.metric_type.value() == "L2");
        CHECK(search_cfg.k.value() == 100);
        CHECK(search_cfg.nprobe.value() == 16);

        knowhere::IvfFlatConfig range_cfg;
        s = knowhere::Config::Load(range_cfg, json, knowhere::RANGE_SEARCH);
        CHECK(s == knowhere::Status::success);
        CHECK(range_cfg.metric_type.value() == "L2");
        CHECK(range_cfg.radius.value() == 1000.0);
        CHECK(range_cfg.range_filter.value() == 1.0);

        knowhere::IvfFlatConfig feder_cfg;
        s = knowhere::Config::Load(feder_cfg, json, knowhere::FEDER);
        CHECK(s == knowhere::Status::success);
        CHECK(range_cfg.trace_visit.value() == true);
    }

    SECTION("check hnsw index config") {
        knowhere::Json json = knowhere::Json::parse(R"({
            "metric_type": "L2",
            "k": 100,
            "M": 32,
            "efConstruction": 100,
            "ef": 116,
            "range_filter": 1.0,
            "radius": 1000.0,
            "trace_visit": true
        })");

        // invalid value check
        {
            knowhere::HnswConfig wrong_cfg;
            auto invalid_value_json = json;
            invalid_value_json["efConstruction"] = 100.10;
            checkBuildConfig(knowhere::IndexEnum::INDEX_HNSW, json);
            s = knowhere::Config::Load(wrong_cfg, invalid_value_json, knowhere::TRAIN);
            CHECK(s == knowhere::Status::type_conflict_in_json);

            invalid_value_json = json;
            invalid_value_json["ef"] = -1;
            s = knowhere::Config::Load(wrong_cfg, invalid_value_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::out_of_range_in_json);

            invalid_value_json = json;
            invalid_value_json["ef"] = nlohmann::json::array({20, 30, 40});
            s = knowhere::Config::Load(wrong_cfg, invalid_value_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::type_conflict_in_json);

            invalid_value_json = json;
            invalid_value_json["ef"] = 99;
            s = knowhere::Config::Load(wrong_cfg, invalid_value_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::out_of_range_in_json);
        }

        knowhere::HnswConfig train_cfg;
        s = knowhere::Config::Load(train_cfg, json, knowhere::TRAIN);
        CHECK(s == knowhere::Status::success);
        CHECK(train_cfg.metric_type.value() == "L2");
        CHECK(train_cfg.M.value() == 32);
        CHECK(train_cfg.efConstruction.value() == 100);

        {
            knowhere::HnswConfig search_cfg;
            s = knowhere::Config::Load(search_cfg, json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }

        {
            knowhere::HnswConfig search_cfg;
            auto search_json = json;
            search_json.erase("ef");
            s = knowhere::Config::Load(search_cfg, search_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
            CHECK_EQ(100, search_cfg.ef.value());
        }

        {
            knowhere::HnswConfig search_cfg;
            auto search_json = json;
            search_json.erase("ef");
            search_json["k"] = 10;
            s = knowhere::Config::Load(search_cfg, search_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
            CHECK_EQ(16, search_cfg.ef.value());
        }

        knowhere::HnswConfig range_cfg;
        s = knowhere::Config::Load(range_cfg, json, knowhere::RANGE_SEARCH);
        CHECK(s == knowhere::Status::success);
        CHECK(range_cfg.metric_type.value() == "L2");
        CHECK(range_cfg.radius.value() == 1000);
        CHECK(range_cfg.range_filter.value() == 1.0);

        knowhere::HnswConfig feder_cfg;
        s = knowhere::Config::Load(feder_cfg, json, knowhere::FEDER);
        CHECK(s == knowhere::Status::success);
        CHECK(range_cfg.trace_visit.value() == true);
        CHECK(range_cfg.overview_levels.value() == 3);
    }
#ifdef KNOWHERE_WITH_DISKANN
    SECTION("check diskann index config") {
        knowhere::Json json = knowhere::Json::parse(R"({
            "metric_type": "L2",
            "k": 100,
            "index_prefix": "tmp",
            "data_path": "/tmp",
            "pq_code_budget_gb": 1.0,
            "build_dram_budget_gb": 1.0,
            "radius": 1000.0 ,
            "range_filter": 1.0,
            "trace_visit": true
        })");
        {
            knowhere::DiskANNConfig train_cfg;
            checkBuildConfig(knowhere::IndexEnum::INDEX_DISKANN, json);
            s = knowhere::Config::Load(train_cfg, json, knowhere::TRAIN);
            CHECK(s == knowhere::Status::success);
            CHECK_EQ(128, train_cfg.search_list_size.value());
            CHECK_EQ("L2", train_cfg.metric_type.value());
        }

        {
            knowhere::DiskANNConfig search_cfg;
            s = knowhere::Config::Load(search_cfg, json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
            CHECK_EQ("L2", search_cfg.metric_type.value());
            CHECK_EQ(100, search_cfg.k.value());
            CHECK_EQ(100, search_cfg.search_list_size.value());
        }

        {
            knowhere::DiskANNConfig search_cfg;
            auto search_json = json;
            search_json["k"] = 2;
            s = knowhere::Config::Load(search_cfg, search_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
            CHECK_EQ(16, search_cfg.search_list_size.value());
        }

        {
            knowhere::DiskANNConfig search_cfg;
            auto search_json = json;
            search_json["search_list_size"] = 99;
            s = knowhere::Config::Load(search_cfg, search_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::out_of_range_in_json);
        }

        knowhere::DiskANNConfig range_cfg;
        s = knowhere::Config::Load(range_cfg, json, knowhere::RANGE_SEARCH);
        CHECK(s == knowhere::Status::success);
        CHECK(range_cfg.metric_type.value() == "L2");
        CHECK(range_cfg.radius.value() == 1000.0);
        CHECK(range_cfg.range_filter.value() == 1.0);

        knowhere::DiskANNConfig feder_cfg;
        s = knowhere::Config::Load(feder_cfg, json, knowhere::FEDER);
        CHECK(s == knowhere::Status::success);
        CHECK(range_cfg.trace_visit.value() == true);
    }
#endif
#ifdef KNOWHERE_WITH_CUVS
    SECTION("check cagra index config") {
        knowhere::Json json = knowhere::Json::parse(R"({
            "metric_type": "L2",
            "k": 100
        })");

        {
            // search without params
            knowhere::GpuRaftCagraConfig cagra_config;
            s = knowhere::Config::Load(cagra_config, json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }

        {
            // search only with legal search_width
            knowhere::GpuRaftCagraConfig cagra_config;
            auto tmp_json = json;
            tmp_json["search_width"] = 4;
            s = knowhere::Config::Load(cagra_config, tmp_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }

        {
            // search only with illegal search_width with default itopk
            knowhere::GpuRaftCagraConfig cagra_config;
            auto tmp_json = json;
            tmp_json["search_width"] = 2;
            s = knowhere::Config::Load(cagra_config, tmp_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }

        {
            // search only with legal itopk
            knowhere::GpuRaftCagraConfig cagra_config;
            auto tmp_json = json;
            tmp_json["itopk_size"] = 120;
            s = knowhere::Config::Load(cagra_config, tmp_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }

        {
            // search only with illegal itopk and default search_width
            knowhere::GpuRaftCagraConfig cagra_config;
            auto tmp_json = json;
            tmp_json["itopk_size"] = 30;
            s = knowhere::Config::Load(cagra_config, tmp_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }

        {
            // search only with illegal itopk and search width
            knowhere::GpuRaftCagraConfig cagra_config;
            auto tmp_json = json;
            tmp_json["itopk_size"] = 30;
            tmp_json["search_width"] = 3;
            s = knowhere::Config::Load(cagra_config, tmp_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::out_of_range_in_json);
        }

        {
            // search only with legal itopk and search width
            knowhere::GpuRaftCagraConfig cagra_config;
            auto tmp_json = json;
            tmp_json["itopk_size"] = 97;
            tmp_json["search_width"] = 2;
            s = knowhere::Config::Load(cagra_config, tmp_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }

        {
            // search only with legal itopk and search width
            knowhere::GpuRaftCagraConfig cagra_config;
            auto tmp_json = json;
            tmp_json["itopk_size"] = 30;
            tmp_json["search_width"] = 4;
            s = knowhere::Config::Load(cagra_config, tmp_json, knowhere::SEARCH);
            CHECK(s == knowhere::Status::success);
        }
    }

#endif
}

TEST_CASE("Test config load", "[BOOL]") {
    knowhere::Status s;
    std::string err_msg;

    SECTION("check bool") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_BOOL bool_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(bool_val).description("bool field for test").for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::invalid_param_in_json);

        json = knowhere::Json::parse(R"({
            "bool_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "bool_val": true
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.bool_val.value() == true);
    }

    SECTION("check bool allow empty") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_BOOL bool_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(bool_val)
                    .description("bool field for test")
                    .allow_empty_without_default()
                    .for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);

        json = knowhere::Json::parse(R"({
            "bool_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "bool_val": true
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.bool_val.value() == true);
    }

    SECTION("check bool with default") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_BOOL bool_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(bool_val)
                    .description("bool field for test")
                    .set_default(true)
                    .for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.bool_val.value() == true);

        json = knowhere::Json::parse(R"({
            "bool_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "bool_val": false
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.bool_val.value() == false);
    }
}

TEST_CASE("Test config load", "[INT]") {
    knowhere::Status s;
    std::string err_msg;

    SECTION("check int") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_INT int_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(int_val).description("int field for test").for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::invalid_param_in_json);

        json = knowhere::Json::parse(R"({
            "int_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "int_val": 10
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.int_val.value() == 10);
    }

    SECTION("check int allow empty") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_INT int_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(int_val)
                    .description("int field for test")
                    .allow_empty_without_default()
                    .for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);

        json = knowhere::Json::parse(R"({
            "int_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "int_val": 10
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.int_val.value() == 10);
    }

    SECTION("check int in range") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_INT int_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(int_val)
                    .description("int field for test")
                    .set_default(2)
                    .for_train_and_search()
                    .set_range(1, 100);
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.int_val.value() == 2);

        json = knowhere::Json::parse(R"({
            "int_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "int_val": 4294967296
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::arithmetic_overflow);

        json = knowhere::Json::parse(R"({
            "int_val": 123
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::out_of_range_in_json);

        json = knowhere::Json::parse(R"({
            "int_val": 10
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.int_val.value() == 10);
    }
}

TEST_CASE("Test config load", "[FLOAT]") {
    knowhere::Status s;
    std::string err_msg;

    SECTION("check float") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_FLOAT float_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(float_val).description("float field for test").for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::invalid_param_in_json);

        json = knowhere::Json::parse(R"({
            "float_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "float_val": 10
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.float_val.value() == 10.0);
    }

    SECTION("check float allow empty") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_FLOAT float_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(float_val)
                    .description("float field for test")
                    .allow_empty_without_default()
                    .for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);

        json = knowhere::Json::parse(R"({
            "float_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "float_val": 10
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.float_val.value() == 10.0);
    }

    SECTION("check float in range") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_FLOAT float_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(float_val)
                    .description("float field for test")
                    .set_default(2.0)
                    .for_train_and_search()
                    .set_range(1.0, 100.0);
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.float_val.value() == 2.0);

        json = knowhere::Json::parse(R"({
            "float_val": "a"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "float_val": 1e+40
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::arithmetic_overflow);

        json = knowhere::Json::parse(R"({
            "float_val": 123
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::out_of_range_in_json);

        json = knowhere::Json::parse(R"({
            "float_val": 10
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.float_val.value() == 10);
    }
}

TEST_CASE("Test config load", "[STRING]") {
    knowhere::Status s;
    std::string err_msg;

    SECTION("check string") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_STRING str_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(str_val).description("string field for test").for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::invalid_param_in_json);

        json = knowhere::Json::parse(R"({
            "str_val": 1
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "str_val": "abc"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.str_val.value() == "abc");
    }

    SECTION("check string allow empty") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_STRING str_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(str_val)
                    .description("string field for test")
                    .allow_empty_without_default()
                    .for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);

        json = knowhere::Json::parse(R"({
            "str_val": 1
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "str_val": "abc"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.str_val.value() == "abc");
    }

    SECTION("check string with default") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_STRING str_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(str_val)
                    .description("string field for test")
                    .set_default("knowhere")
                    .for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.str_val.value() == "knowhere");

        json = knowhere::Json::parse(R"({
            "str_val": 1
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::type_conflict_in_json);

        json = knowhere::Json::parse(R"({
            "str_val": "abc"
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.str_val.value() == "abc");
    }
}

TEST_CASE("Test config load", "[MATERIALIZED_VIEW_SEARCH_INFO]") {
    knowhere::Status s;
    std::string err_msg;

    SECTION("check string") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_MATERIALIZED_VIEW_SEARCH_INFO_TYPE info_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(info_val).description("info field for test").for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::invalid_param_in_json);

        json = knowhere::Json::parse(R"({
            "info_val": ""
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
    }

    SECTION("check string allow empty") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_MATERIALIZED_VIEW_SEARCH_INFO_TYPE info_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(info_val)
                    .description("info field for test")
                    .allow_empty_without_default()
                    .for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({})");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);

        json = knowhere::Json::parse(R"({
            "info_val": ""
        })");
        s = knowhere::Config::Load(test_cfg, json, knowhere::TRAIN, &err_msg);
        CHECK(s == knowhere::Status::success);
    }
}

TEST_CASE("Test config", "[FormatAndCheck]") {
    knowhere::Status s;
    std::string err_msg;

    SECTION("check config with string type values") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_INT int_val;
            CFG_FLOAT float_val;
            CFG_BOOL true_val;
            CFG_BOOL false_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(int_val).for_train_and_search();
                KNOWHERE_CONFIG_DECLARE_FIELD(float_val).for_train_and_search();
                KNOWHERE_CONFIG_DECLARE_FIELD(true_val).for_train_and_search();
                KNOWHERE_CONFIG_DECLARE_FIELD(false_val).for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({
            "int_val": "123",
            "float_val": "1.23",
            "true_val": "true",
            "false_val": "false"
        })");
        s = knowhere::Config::FormatAndCheck(test_cfg, json, &err_msg);
        CHECK(s == knowhere::Status::success);
        s = knowhere::Config::Load(test_cfg, json, knowhere::SEARCH, &err_msg);
        CHECK(s == knowhere::Status::success);
        CHECK(test_cfg.int_val.value() == 123);
        CHECK_LT(std::abs(test_cfg.float_val.value() - 1.23), 0.00001);
        CHECK(test_cfg.true_val.value() == true);
        CHECK(test_cfg.false_val.value() == false);
    }

    SECTION("check config with invalid string type int value") {
        class TestConfig : public knowhere::Config {
         public:
            CFG_INT int_val;
            KNOHWERE_DECLARE_CONFIG(TestConfig) {
                KNOWHERE_CONFIG_DECLARE_FIELD(int_val).for_train_and_search();
            }
        };

        TestConfig test_cfg;
        knowhere::Json json;

        json = knowhere::Json::parse(R"({
            "int_val": "12.3"
        })");
        s = knowhere::Config::FormatAndCheck(test_cfg, json, &err_msg);
        CHECK(s == knowhere::Status::invalid_value_in_json);
    }
}
