// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "benchmark_knowhere.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/comp/knowhere_config.h"
#include "knowhere/comp/local_file_manager.h"
#include "knowhere/dataset.h"

const int32_t GPU_DEVICE_ID = 0;

class Benchmark_float_qps : public Benchmark_knowhere, public ::testing::Test {
 public:
    template <typename T>
    void
    test_idmap(const knowhere::Json& cfg) {
        auto conf = cfg;

        float expected_recall = 1.0f;
        conf[knowhere::meta::TOPK] = topk_;

        std::string data_type_str = get_data_type_name<T>();
        printf("\n[%0.3f s] %s | %s(%s) | k=%d, R@=%.4f\n", get_time_diff(), ann_test_name_.c_str(),
               index_type_.c_str(), data_type_str.c_str(), topk_, expected_recall);
        printf("================================================================================\n");
        for (auto thread_num : THREAD_NUMs_) {
            CALC_TIME_SPAN(task<T>(conf, thread_num, nq_));
            printf("  thread_num = %2d, elapse = %6.3fs, VPS = %.3f\n", thread_num, TDIFF_, nq_ / TDIFF_);
            std::fflush(stdout);
        }
        printf("================================================================================\n");
        printf("[%.3f s] Test '%s/%s' done\n\n", get_time_diff(), ann_test_name_.c_str(), index_type_.c_str());
    }

    template <typename T>
    void
    test_ivf(const knowhere::Json& cfg) {
        auto conf = cfg;
        auto nlist = conf[knowhere::indexparam::NLIST].get<int32_t>();
        std::string data_type_str = get_data_type_name<T>();

        auto find_smallest_nprobe = [&](float expected_recall) -> std::tuple<int32_t, float> {
            std::unordered_map<int32_t, float> recall_map;
            conf[knowhere::meta::TOPK] = topk_;
            auto ds_ptr = knowhere::GenDataSet(nq_, dim_, xq_);
            auto query = knowhere::ConvertToDataTypeIfNeeded<T>(ds_ptr);

            int32_t left = 1, right = 256, nprobe;
            float recall;
            while (left <= right) {
                nprobe = left + (right - left) / 2;
                conf[knowhere::indexparam::NPROBE] = nprobe;

                auto result = index_.value().Search(query, conf, nullptr);
                recall = CalcRecall(result.value()->GetIds(), nq_, topk_);
                recall_map[nprobe] = recall;
                printf("[%0.3f s] iterate IVF param for recall %.4f: nlist=%d, nprobe=%4d, k=%d, R@=%.4f\n",
                       get_time_diff(), expected_recall, nlist, nprobe, topk_, recall);
                std::fflush(stdout);
                if (std::abs(recall - expected_recall) <= 0.0001) {
                    return {nprobe, recall_map[nprobe]};
                }
                if (recall < expected_recall) {
                    left = nprobe + 1;
                } else {
                    right = nprobe - 1;
                }
            }
            return {left, recall_map[left]};
        };

        for (auto expected_recall : EXPECTED_RECALLs_) {
            auto [nprobe, recall] = find_smallest_nprobe(expected_recall);
            conf[knowhere::indexparam::NPROBE] = nprobe;
            conf[knowhere::meta::TOPK] = topk_;

            printf("\n[%0.3f s] %s | %s(%s) | nlist=%d, nprobe=%d, k=%d, R@=%.4f\n", get_time_diff(),
                   ann_test_name_.c_str(), index_type_.c_str(), data_type_str.c_str(), nlist, nprobe, topk_, recall);
            printf("================================================================================\n");
            for (auto thread_num : THREAD_NUMs_) {
                CALC_TIME_SPAN(task<T>(conf, thread_num, nq_));
                printf("  thread_num = %2d, elapse = %6.3fs, VPS = %.3f\n", thread_num, TDIFF_, nq_ / TDIFF_);
                std::fflush(stdout);
            }
            printf("================================================================================\n");
            printf("[%.3f s] Test '%s/%s' done\n\n", get_time_diff(), ann_test_name_.c_str(), index_type_.c_str());
        }
    }

    template <typename T>
    void
    test_raft_cagra(const knowhere::Json& cfg) {
        auto conf = cfg;
        std::string data_type_str = get_data_type_name<T>();

        auto find_smallest_itopk_size = [&](float expected_recall) -> std::tuple<int32_t, float> {
            std::unordered_map<int32_t, float> recall_map;
            auto ds_ptr = knowhere::GenDataSet(nq_, dim_, xq_);
            auto left = 32;
            auto right = 256;
            auto itopk_size = left;

            float recall;
            while (left <= right) {
                itopk_size = left + (right - left) / 2;
                conf[knowhere::indexparam::ITOPK_SIZE] = itopk_size;

                auto result = index_.value().Search(ds_ptr, conf, nullptr);
                recall = CalcRecall(result.value()->GetIds(), nq_, topk_);
                recall_map[itopk_size] = recall;
                printf("[%0.3f s] iterate CAGRA param for recall %.4f: itopk_size=%d, k=%d, R@=%.4f\n", get_time_diff(),
                       expected_recall, itopk_size, topk_, recall);
                std::fflush(stdout);
                if (std::abs(recall - expected_recall) <= 0.0001) {
                    return {itopk_size, recall_map[itopk_size]};
                }
                if (recall < expected_recall) {
                    left = itopk_size + 1;
                } else {
                    right = itopk_size - 1;
                }
            }
            return {left, recall_map[itopk_size]};
        };

        for (auto expected_recall : EXPECTED_RECALLs_) {
            auto [itopk_size, recall] = find_smallest_itopk_size(expected_recall);
            conf[knowhere::indexparam::ITOPK_SIZE] = ((int{topk_} + 32 - 1) / 32) * 32;
            conf[knowhere::meta::TOPK] = topk_;
            conf[knowhere::indexparam::ITOPK_SIZE] = itopk_size;

            printf("\n[%0.3f s] %s | %s(%s) | k=%d, R@=%.4f\n", get_time_diff(), ann_test_name_.c_str(),
                   index_type_.c_str(), data_type_str.c_str(), topk_, recall);
            printf("================================================================================\n");
            for (auto thread_num : THREAD_NUMs_) {
                CALC_TIME_SPAN(task<T>(conf, thread_num, nq_));
                printf("  thread_num = %2d, elapse = %6.3fs, VPS = %.3f\n", thread_num, TDIFF_, nq_ / TDIFF_);
                std::fflush(stdout);
            }
            printf("================================================================================\n");
            printf("[%.3f s] Test '%s/%s' done\n\n", get_time_diff(), ann_test_name_.c_str(), index_type_.c_str());
        }
    }

    template <typename T>
    void
    test_hnsw(const knowhere::Json& cfg) {
        auto conf = cfg;
        auto M = conf[knowhere::indexparam::HNSW_M].get<int32_t>();
        auto efConstruction = conf[knowhere::indexparam::EFCONSTRUCTION].get<int32_t>();
        std::string data_type_str = get_data_type_name<T>();

        auto find_smallest_ef = [&](float expected_recall) -> std::tuple<int32_t, float> {
            std::unordered_map<int32_t, float> recall_map;
            conf[knowhere::meta::TOPK] = topk_;
            auto ds_ptr = knowhere::GenDataSet(nq_, dim_, xq_);
            auto query = knowhere::ConvertToDataTypeIfNeeded<T>(ds_ptr);

            int32_t left = topk_, right = 1024, ef;
            float recall;
            while (left <= right) {
                ef = left + (right - left) / 2;
                conf[knowhere::indexparam::EF] = ef;

                auto result = index_.value().Search(query, conf, nullptr);
                recall = CalcRecall(result.value()->GetIds(), nq_, topk_);
                recall_map[ef] = recall;
                printf("[%0.3f s] iterate HNSW param for expected recall %.4f: M=%d, efc=%d, ef=%4d, k=%d, R@=%.4f\n",
                       get_time_diff(), expected_recall, M, efConstruction, ef, topk_, recall);
                std::fflush(stdout);
                if (std::abs(recall - expected_recall) <= 0.0001) {
                    return {ef, recall_map[ef]};
                }
                if (recall < expected_recall) {
                    left = ef + 1;
                } else {
                    right = ef - 1;
                }
            }
            return {left, recall_map[left]};
        };

        for (auto expected_recall : EXPECTED_RECALLs_) {
            auto [ef, recall] = find_smallest_ef(expected_recall);
            conf[knowhere::indexparam::EF] = ef;
            conf[knowhere::meta::TOPK] = topk_;

            printf("\n[%0.3f s] %s | %s(%s) | M=%d | efConstruction=%d, ef=%d, k=%d, R@=%.4f\n", get_time_diff(),
                   ann_test_name_.c_str(), index_type_.c_str(), data_type_str.c_str(), M, efConstruction, ef, topk_,
                   recall);
            printf("================================================================================\n");
            for (auto thread_num : THREAD_NUMs_) {
                CALC_TIME_SPAN(task<T>(conf, thread_num, nq_));
                printf("  thread_num = %2d, elapse = %6.3fs, VPS = %.3f\n", thread_num, TDIFF_, nq_ / TDIFF_);
                std::fflush(stdout);
            }
            printf("================================================================================\n");
            printf("[%.3f s] Test '%s/%s' done\n\n", get_time_diff(), ann_test_name_.c_str(), index_type_.c_str());
        }
    }

    template <typename T>
    void
    test_scann(const knowhere::Json& cfg) {
        auto conf = cfg;

        const auto reorder_k = conf[knowhere::indexparam::REORDER_K].get<int32_t>();
        const auto with_raw_data = conf[knowhere::indexparam::WITH_RAW_DATA].get<bool>();
        auto nlist = conf[knowhere::indexparam::NLIST].get<int32_t>();
        std::string data_type_str = get_data_type_name<T>();

        auto find_smallest_nprobe = [&](float expected_recall) -> std::tuple<int32_t, float> {
            std::unordered_map<int32_t, float> recall_map;
            conf[knowhere::meta::TOPK] = topk_;
            auto ds_ptr = knowhere::GenDataSet(nq_, dim_, xq_);
            auto query = knowhere::ConvertToDataTypeIfNeeded<T>(ds_ptr);

            int32_t left = 1, right = 256, nprobe;
            float recall;
            while (left <= right) {
                nprobe = left + (right - left) / 2;
                conf[knowhere::indexparam::NPROBE] = nprobe;

                auto result = index_.value().Search(query, conf, nullptr);
                recall = CalcRecall(result.value()->GetIds(), nq_, topk_);
                recall_map[nprobe] = recall;
                printf(
                    "[%0.3f s] iterate scann param for recall %.4f: nlist=%d, nprobe=%4d, reorder_k=%d, "
                    "with_raw_data=%d, k=%d, R@=%.4f\n",
                    get_time_diff(), expected_recall, nlist, nprobe, reorder_k, with_raw_data ? 1 : 0, topk_, recall);
                std::fflush(stdout);
                if (std::abs(recall - expected_recall) <= 0.0001) {
                    return {nprobe, recall_map[nprobe]};
                }
                if (recall < expected_recall) {
                    left = nprobe + 1;
                } else {
                    right = nprobe - 1;
                }
            }
            return {left, recall_map[left]};
        };

        for (auto expected_recall : EXPECTED_RECALLs_) {
            auto [nprobe, recall] = find_smallest_nprobe(expected_recall);
            conf[knowhere::indexparam::NPROBE] = nprobe;
            conf[knowhere::meta::TOPK] = topk_;

            printf("\n[%0.3f s] %s | %s(%s) | nlist=%d, nprobe=%d, reorder_k=%d, with_raw_data=%d, k=%d, R@=%.4f\n",
                   get_time_diff(), ann_test_name_.c_str(), index_type_.c_str(), data_type_str.c_str(), nlist, nprobe,
                   reorder_k, with_raw_data ? 1 : 0, topk_, recall);
            printf("================================================================================\n");
            for (auto thread_num : THREAD_NUMs_) {
                CALC_TIME_SPAN(task<T>(conf, thread_num, nq_));
                printf("  thread_num = %2d, elapse = %6.3fs, VPS = %.3f\n", thread_num, TDIFF_, nq_ / TDIFF_);
                std::fflush(stdout);
            }
            printf("================================================================================\n");
            printf("[%.3f s] Test '%s/%s' done\n\n", get_time_diff(), ann_test_name_.c_str(), index_type_.c_str());
        }
    }

#ifdef KNOWHERE_WITH_DISKANN
    template <typename T>
    void
    test_diskann(const knowhere::Json& cfg) {
        auto conf = cfg;
        std::string data_type_str = get_data_type_name<T>();

        auto find_smallest_search_list_size = [&](float expected_recall) -> std::tuple<int32_t, float> {
            std::unordered_map<int32_t, float> recall_map;
            conf[knowhere::meta::TOPK] = topk_;
            auto ds_ptr = knowhere::GenDataSet(nq_, dim_, xq_);
            auto query = knowhere::ConvertToDataTypeIfNeeded<T>(ds_ptr);

            int32_t left = topk_, right = 512, search_list_size;
            float recall;
            while (left <= right) {
                search_list_size = left + (right - left) / 2;
                conf[knowhere::indexparam::SEARCH_LIST_SIZE] = search_list_size;

                auto result = index_.value().Search(query, conf, nullptr);
                recall = CalcRecall(result.value()->GetIds(), nq_, topk_);
                recall_map[search_list_size] = recall;
                printf(
                    "[%0.3f s] iterate DISKANN param for expected recall %.4f: search_list_size=%4d, k=%d, R@=%.4f\n",
                    get_time_diff(), expected_recall, search_list_size, topk_, recall);
                std::fflush(stdout);
                if (std::abs(recall - expected_recall) <= 0.0001) {
                    return {search_list_size, recall_map[search_list_size]};
                }
                if (recall < expected_recall) {
                    left = search_list_size + 1;
                } else {
                    right = search_list_size - 1;
                }
            }
            return {left, recall_map[left]};
        };

        for (auto expected_recall : EXPECTED_RECALLs_) {
            auto [search_list_size, recall] = find_smallest_search_list_size(expected_recall);
            conf[knowhere::indexparam::SEARCH_LIST_SIZE] = search_list_size;
            conf[knowhere::meta::TOPK] = topk_;

            printf("\n[%0.3f s] %s | %s(%s) | search_list_size=%d, k=%d, R@=%.4f\n", get_time_diff(),
                   ann_test_name_.c_str(), index_type_.c_str(), data_type_str.c_str(), search_list_size, topk_, recall);
            printf("================================================================================\n");
            for (auto thread_num : THREAD_NUMs_) {
                CALC_TIME_SPAN(task<T>(conf, thread_num, nq_));
                printf("  thread_num = %2d, elapse = %6.3fs, VPS = %.3f\n", thread_num, TDIFF_, nq_ / TDIFF_);
                std::fflush(stdout);
            }
            printf("================================================================================\n");
            printf("[%.3f s] Test '%s/%s' done\n\n", get_time_diff(), ann_test_name_.c_str(), index_type_.c_str());
        }
    }
#endif

 private:
    template <typename T>
    void
    task(const knowhere::Json& conf, int32_t worker_num, int32_t nq_total) {
        auto worker = [&](int32_t idx_start, int32_t num) {
            num = std::min(num, nq_total - idx_start);
            for (int32_t i = 0; i < num; i++) {
                auto ds_ptr = knowhere::GenDataSet(1, dim_, (const float*)xq_ + (idx_start + i) * dim_);
                auto query = knowhere::ConvertToDataTypeIfNeeded<T>(ds_ptr);
                index_.value().Search(query, conf, nullptr);
            }
        };

        std::vector<std::thread> thread_vector(worker_num);
        for (int32_t i = 0; i < worker_num; i++) {
            int32_t idx_start, req_num;
            req_num = nq_total / worker_num;
            if (nq_total % worker_num != 0) {
                req_num++;
            }
            idx_start = req_num * i;
            thread_vector[i] = std::thread(worker, idx_start, req_num);
        }
        for (int32_t i = 0; i < worker_num; i++) {
            thread_vector[i].join();
        }
    }

 protected:
    void
    SetUp() override {
        T0_ = elapsed();
        set_ann_test_name("sift-128-euclidean");
        parse_ann_test_name();
        load_hdf5_data<false>();

        cfg_[knowhere::meta::METRIC_TYPE] = metric_type_;
        knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::AVX2);
        knowhere::KnowhereConfig::SetBuildThreadPoolSize(default_build_thread_num);
        knowhere::KnowhereConfig::SetSearchThreadPoolSize(default_search_thread_num);
        printf("faiss::distance_compute_blas_threshold: %ld\n", knowhere::KnowhereConfig::GetBlasThreshold());
#ifdef KNOWHERE_WITH_GPU
        knowhere::KnowhereConfig::InitGPUResource(GPU_DEVICE_ID, 2);
        cfg_[knowhere::meta::DEVICE_ID] = GPU_DEVICE_ID;
#endif
#ifdef KNOWHERE_WITH_CUVS
        knowhere::KnowhereConfig::SetRaftMemPool();
#endif
    }

    void
    TearDown() override {
        free_all();
#ifdef KNOWHERE_WITH_GPU
        knowhere::KnowhereConfig::FreeGPUResource();
#endif
    }

 protected:
    const int32_t topk_ = 100;
    const std::vector<float> EXPECTED_RECALLs_ = {0.8, 0.95};
    const std::vector<int32_t> THREAD_NUMs_ = {1, 2, 4, 8};

    // IVF index params
    const std::vector<int32_t> NLISTs_ = {1024};

    // IVFPQ index params
    const std::vector<int32_t> Ms_ = {8, 16, 32};
    const int32_t NBITS_ = 8;

    // HNSW index params
    const std::vector<int32_t> HNSW_Ms_ = {16};
    const std::vector<int32_t> EFCONs_ = {100};

    // SCANN index params
    const std::vector<int32_t> SCANN_REORDER_K = {256, 512, 768, 1024};
    const std::vector<bool> SCANN_WITH_RAW_DATA = {true};

    // CAGRA index params
    const std::vector<int32_t> GRAPH_DEGREEs_ = {8, 16, 32};
    const std::vector<int32_t> ITOPK_SIZEs_ = {128, 192, 256};
};

#define TEST_INDEX(NAME, T, X)              \
    index_file_name = get_index_name<T>(X); \
    create_index<T>(index_file_name, conf); \
    test_##NAME<T>(conf)

TEST_F(Benchmark_float_qps, TEST_IDMAP) {
    index_type_ = knowhere::IndexEnum::INDEX_FAISS_IDMAP;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    std::vector<int32_t> params = {};

    TEST_INDEX(idmap, knowhere::fp32, params);
    TEST_INDEX(idmap, knowhere::fp16, params);
    TEST_INDEX(idmap, knowhere::bf16, params);
}

TEST_F(Benchmark_float_qps, TEST_IVF_FLAT) {
    index_type_ = knowhere::IndexEnum::INDEX_FAISS_IVFFLAT;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    for (auto nlist : NLISTs_) {
        conf[knowhere::indexparam::NLIST] = nlist;
        std::vector<int32_t> params = {nlist};

        TEST_INDEX(ivf, knowhere::fp32, params);
        TEST_INDEX(ivf, knowhere::fp16, params);
        TEST_INDEX(ivf, knowhere::bf16, params);
    }
}

TEST_F(Benchmark_float_qps, TEST_IVF_SQ8) {
    index_type_ = knowhere::IndexEnum::INDEX_FAISS_IVFSQ8;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    for (auto nlist : NLISTs_) {
        conf[knowhere::indexparam::NLIST] = nlist;
        std::vector<int32_t> params = {nlist};

        TEST_INDEX(ivf, knowhere::fp32, params);
        TEST_INDEX(ivf, knowhere::fp16, params);
        TEST_INDEX(ivf, knowhere::bf16, params);
    }
}

TEST_F(Benchmark_float_qps, TEST_IVF_PQ) {
    index_type_ = knowhere::IndexEnum::INDEX_FAISS_IVFPQ;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    conf[knowhere::indexparam::NBITS] = NBITS_;
    for (auto m : Ms_) {
        conf[knowhere::indexparam::M] = m;
        for (auto nlist : NLISTs_) {
            conf[knowhere::indexparam::NLIST] = nlist;
            std::vector<int32_t> params = {nlist, m};

            TEST_INDEX(ivf, knowhere::fp32, params);
            TEST_INDEX(ivf, knowhere::fp16, params);
            TEST_INDEX(ivf, knowhere::bf16, params);
        }
    }
}

TEST_F(Benchmark_float_qps, TEST_HNSW) {
    index_type_ = knowhere::IndexEnum::INDEX_HNSW;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    for (auto M : HNSW_Ms_) {
        conf[knowhere::indexparam::HNSW_M] = M;
        for (auto efc : EFCONs_) {
            conf[knowhere::indexparam::EFCONSTRUCTION] = efc;
            std::vector<int32_t> params = {M, efc};

            TEST_INDEX(hnsw, knowhere::fp32, params);
            TEST_INDEX(hnsw, knowhere::fp16, params);
            TEST_INDEX(hnsw, knowhere::bf16, params);
        }
    }
}

TEST_F(Benchmark_float_qps, TEST_SCANN) {
    index_type_ = knowhere::IndexEnum::INDEX_FAISS_SCANN;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    for (auto reorder_k : SCANN_REORDER_K) {
        if (reorder_k < topk_) {
            continue;
        }
        conf[knowhere::indexparam::REORDER_K] = reorder_k;
        for (auto nlist : NLISTs_) {
            conf[knowhere::indexparam::NLIST] = nlist;
            for (const auto with_raw_data : SCANN_WITH_RAW_DATA) {
                conf[knowhere::indexparam::WITH_RAW_DATA] = with_raw_data;
                std::vector<int32_t> params = {nlist, reorder_k, with_raw_data};

                TEST_INDEX(scann, knowhere::fp32, params);
                TEST_INDEX(scann, knowhere::fp16, params);
                TEST_INDEX(scann, knowhere::bf16, params);
            }
        }
    }
}

#ifdef KNOWHERE_WITH_DISKANN
TEST_F(Benchmark_float_qps, TEST_DISKANN) {
    index_type_ = knowhere::IndexEnum::INDEX_DISKANN;

    knowhere::Json conf = cfg_;

    conf[knowhere::meta::INDEX_PREFIX] = (metric_type_ == knowhere::metric::L2 ? kL2IndexPrefix : kIPIndexPrefix);
    conf[knowhere::meta::DATA_PATH] = kRawDataPath;
    conf[knowhere::indexparam::MAX_DEGREE] = 56;
    conf[knowhere::indexparam::PQ_CODE_BUDGET_GB] = sizeof(float) * dim_ * nb_ * 0.125 / (1024 * 1024 * 1024);
    conf[knowhere::indexparam::BUILD_DRAM_BUDGET_GB] = 32.0;
    conf[knowhere::indexparam::SEARCH_CACHE_BUDGET_GB] = 0;
    conf[knowhere::indexparam::BEAMWIDTH] = 8;

    fs::create_directory(kDir);
    fs::create_directory(kL2IndexDir);
    fs::create_directory(kIPIndexDir);

    WriteRawDataToDisk(kRawDataPath, (const float*)xb_, (const uint32_t)nb_, (const uint32_t)dim_);

    std::shared_ptr<knowhere::FileManager> file_manager = std::make_shared<knowhere::LocalFileManager>();
    auto diskann_index_pack = knowhere::Pack(file_manager);

    index_ = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(
        index_type_, knowhere::Version::GetCurrentVersion().VersionNumber(), diskann_index_pack);
    printf("[%.3f s] Building all on %d vectors\n", get_time_diff(), nb_);
    knowhere::DataSetPtr ds_ptr = nullptr;
    index_.value().Build(ds_ptr, conf);

    knowhere::BinarySet binset;
    index_.value().Serialize(binset);
    index_.value().Deserialize(binset, conf);

    test_diskann<knowhere::fp32>(conf);
}
#endif

#ifdef KNOWHERE_WITH_RAFT
TEST_F(Benchmark_float_qps, TEST_RAFT_BRUTE_FORCE) {
    index_type_ = knowhere::IndexEnum::INDEX_RAFT_BRUTEFORCE;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    std::vector<int32_t> params = {};

    TEST_INDEX(idmap, knowhere::fp32, params);
}

TEST_F(Benchmark_float_qps, TEST_RAFT_IVF_FLAT) {
    index_type_ = knowhere::IndexEnum::INDEX_RAFT_IVFFLAT;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    for (auto nlist : NLISTs_) {
        conf[knowhere::indexparam::NLIST] = nlist;
        std::vector<int32_t> params = {nlist};

        TEST_INDEX(ivf, knowhere::fp32, params);
    }
}

TEST_F(Benchmark_float_qps, TEST_RAFT_IVF_PQ) {
    index_type_ = knowhere::IndexEnum::INDEX_RAFT_IVFPQ;

    std::string index_file_name;
    knowhere::Json conf = cfg_;
    conf[knowhere::indexparam::NBITS] = NBITS_;
    for (auto m : Ms_) {
        conf[knowhere::indexparam::M] = m;
        for (auto nlist : NLISTs_) {
            conf[knowhere::indexparam::NLIST] = nlist;
            std::vector<int32_t> params = {nlist, m};

            TEST_INDEX(ivf, knowhere::fp32, params);
        }
    }
}

TEST_F(Benchmark_float_qps, TEST_RAFT_CAGRA) {
    index_type_ = knowhere::IndexEnum::INDEX_RAFT_CAGRA;

    std::string index_file_name;
    knowhere::Json conf = cfg_;

    for (auto graph_degree : GRAPH_DEGREEs_) {
        conf[knowhere::indexparam::GRAPH_DEGREE] = graph_degree;
        conf[knowhere::indexparam::INTERMEDIATE_GRAPH_DEGREE] = graph_degree;
        std::vector<int32_t> params = {graph_degree};
        TEST_INDEX(raft_cagra, knowhere::fp32, params);
    }
}
#endif
