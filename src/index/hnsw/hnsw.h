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

#pragma once

#include <new>
#include <numeric>

#include "hnswlib/hnswalg.h"
#include "hnswlib/hnswlib.h"
#include "index/hnsw/hnsw_config.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/comp/thread_pool.h"
#include "knowhere/comp/time_recorder.h"
#include "knowhere/config.h"
#include "knowhere/expected.h"
#include "knowhere/feature.h"
#include "knowhere/feder/HNSW.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/index/index_node_data_mock_wrapper.h"
#include "knowhere/log.h"
#include "knowhere/range_util.h"
#include "knowhere/utils.h"

namespace knowhere {

using hnswlib::QuantType;

template <typename DataType, QuantType quant_type = QuantType::None>
class HnswIndexNode : public IndexNode {
 public:
    using DistType = float;
    HnswIndexNode(const int32_t& /*version*/, const Object& object) : index_(nullptr) {
        search_pool_ = ThreadPool::GetGlobalSearchThreadPool();
    }

    Status
    Train(const DataSetPtr dataset, std::shared_ptr<Config> cfg) override {
        auto rows = dataset->GetRows();
        auto dim = dataset->GetDim();
        auto hnsw_cfg = static_cast<const BaseHnswConfig&>(*cfg);
        hnswlib::SpaceInterface<DistType>* space = nullptr;
        if constexpr (KnowhereFloatTypeCheck<DataType>::value) {
            if (IsMetricType(hnsw_cfg.metric_type.value(), metric::L2)) {
                space = new (std::nothrow) hnswlib::L2Space<DataType, DistType>(dim);
            } else if (IsMetricType(hnsw_cfg.metric_type.value(), metric::IP)) {
                space = new (std::nothrow) hnswlib::InnerProductSpace<DataType, DistType>(dim);
            } else if (IsMetricType(hnsw_cfg.metric_type.value(), metric::COSINE)) {
                space = new (std::nothrow) hnswlib::CosineSpace<DataType, DistType>(dim);
            } else {
                LOG_KNOWHERE_WARNING_
                    << "metric type and data type(float32, float16 and bfloat16) are not match in hnsw: "
                    << hnsw_cfg.metric_type.value();
                return Status::invalid_metric_type;
            }
        } else {
            if (IsMetricType(hnsw_cfg.metric_type.value(), metric::HAMMING)) {
                space = new (std::nothrow) hnswlib::HammingSpace(dim);
            } else if (IsMetricType(hnsw_cfg.metric_type.value(), metric::JACCARD)) {
                space = new (std::nothrow) hnswlib::JaccardSpace(dim);
            } else {
                LOG_KNOWHERE_WARNING_ << "metric type and data type(binary) are not match in hnsw: "
                                      << hnsw_cfg.metric_type.value();
                return Status::invalid_metric_type;
            }
        }

        auto index = new (std::nothrow) hnswlib::HierarchicalNSW<DataType, DistType, quant_type>(
            space, rows, hnsw_cfg.M.value(), hnsw_cfg.efConstruction.value());
        if (index == nullptr) {
            LOG_KNOWHERE_WARNING_ << "memory malloc error.";
            return Status::malloc_error;
        }
        if (this->index_) {
            delete this->index_;
            LOG_KNOWHERE_WARNING_ << "index not empty, deleted old index";
        }
        this->index_ = index;
        if constexpr (quant_type != QuantType::None) {
            this->index_->trainSQuant((const DataType*)dataset->GetTensor(), rows);
        }
        return Status::success;
    }

    static Status
    StaticConfigCheck(const Config& cfg, PARAM_TYPE paramType, std::string& msg) {
        auto hnsw_cfg = static_cast<const BaseHnswConfig&>(cfg);

        if (paramType == PARAM_TYPE::TRAIN) {
            if constexpr (KnowhereFloatTypeCheck<DataType>::value) {
                if (IsMetricType(hnsw_cfg.metric_type.value(), metric::L2) ||
                    IsMetricType(hnsw_cfg.metric_type.value(), metric::IP) ||
                    IsMetricType(hnsw_cfg.metric_type.value(), metric::COSINE)) {
                } else {
                    msg = "metric type " + hnsw_cfg.metric_type.value() +
                          " not found or not supported, supported: [L2 IP COSINE]";
                    return Status::invalid_metric_type;
                }
            } else {
                if (IsMetricType(hnsw_cfg.metric_type.value(), metric::HAMMING) ||
                    IsMetricType(hnsw_cfg.metric_type.value(), metric::JACCARD)) {
                } else {
                    msg = "metric type " + hnsw_cfg.metric_type.value() +
                          " not found or not supported, supported: [HAMMING JACCARD]";
                    return Status::invalid_metric_type;
                }
            }
        }
        return Status::success;
    }

    Status
    Add(const DataSetPtr dataset, std::shared_ptr<Config> cfg) override {
        if (!index_) {
            LOG_KNOWHERE_ERROR_ << "Can not add data to empty HNSW index.";
            return Status::empty_index;
        }

        knowhere::TimeRecorder build_time("Building HNSW cost", 2);
        auto rows = dataset->GetRows();
        if (rows <= 0) {
            LOG_KNOWHERE_ERROR_ << "Can not add empty data to HNSW index.";
            return Status::empty_index;
        }
        auto tensor = dataset->GetTensor();
        auto hnsw_cfg = static_cast<const BaseHnswConfig&>(*cfg);
        bool shuffle_build = hnsw_cfg.shuffle_build.value();

        std::atomic<uint64_t> counter{0};
        uint64_t one_tenth_row = rows / 10;

        std::vector<int> shuffle_batch_ids;
        constexpr int64_t batch_size = 8192;  // same with diskann
        int64_t round_num = std::ceil(float(rows - 1) / batch_size);
        auto build_pool = ThreadPool::GetGlobalBuildThreadPool();
        std::vector<folly::Future<folly::Unit>> futures;

        if (shuffle_build) {
            shuffle_batch_ids.reserve(round_num);
            for (int i = 0; i < round_num; ++i) {
                shuffle_batch_ids.emplace_back(i);
            }
            std::random_device rng;
            std::mt19937 urng(rng());
            std::shuffle(shuffle_batch_ids.begin(), shuffle_batch_ids.end(), urng);
        }
        try {
            index_->addPoint(tensor, 0);

            futures.reserve(batch_size);
            for (int64_t round_id = 0; round_id < round_num; round_id++) {
                int64_t start_id = (shuffle_build ? shuffle_batch_ids[round_id] : round_id) * batch_size;
                int64_t end_id =
                    std::min(rows - 1, ((shuffle_build ? shuffle_batch_ids[round_id] : round_id) + 1) * batch_size);
                for (int64_t i = start_id; i < end_id; ++i) {
                    futures.emplace_back(build_pool->push([&, idx = i + 1]() {
                        index_->addPoint(((const char*)tensor + index_->data_size_ * idx), idx);
                        uint64_t added = counter.fetch_add(1);
                        if (added % one_tenth_row == 0) {
                            LOG_KNOWHERE_INFO_ << "HNSW build progress: " << (added / one_tenth_row) << "0%";
                        }
                    }));
                }
                WaitAllSuccess(futures);
                futures.clear();
            }

            build_time.RecordSection("graph build");
            std::vector<unsigned> unreached = index_->findUnreachableVectors();
            int unreached_num = unreached.size();
            LOG_KNOWHERE_INFO_ << "there are " << unreached_num << " points can not be reached";
            if (unreached_num > 0) {
                futures.reserve(unreached_num);
                for (int i = 0; i < unreached_num; ++i) {
                    futures.emplace_back(
                        build_pool->push([&, idx = i]() { index_->repairGraphConnectivity(unreached[idx]); }));
                }
                WaitAllSuccess(futures);
            }
            build_time.RecordSection("graph repair");
            LOG_KNOWHERE_INFO_ << "HNSW built with #points num:" << index_->max_elements_ << " #M:" << index_->M_
                               << " #max level:" << index_->maxlevel_
                               << " #ef_construction:" << index_->ef_construction_
                               << " #dim:" << *(size_t*)(index_->space_->get_dist_func_param());
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "hnsw inner error: " << e.what();
            return Status::hnsw_inner_error;
        }
        return Status::success;
    }

    expected<DataSetPtr>
    Search(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "search on empty index";
            return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
        }
        auto nq = dataset->GetRows();
        auto xq = dataset->GetTensor();

        auto hnsw_cfg = static_cast<const BaseHnswConfig&>(*cfg);
        auto k = hnsw_cfg.k.value();

        feder::hnsw::FederResultUniq feder_result;
        if (hnsw_cfg.trace_visit.value()) {
            if (nq != 1) {
                return expected<DataSetPtr>::Err(Status::invalid_args, "nq must be 1");
            }
            feder_result = std::make_unique<feder::hnsw::FederResult>();
        }

        auto p_id = std::make_unique<int64_t[]>(k * nq);
        auto p_dist = std::make_unique<DistType[]>(k * nq);

        hnswlib::SearchParam param{(size_t)hnsw_cfg.ef.value()};
        bool transform =
            (index_->metric_type_ == hnswlib::Metric::INNER_PRODUCT || index_->metric_type_ == hnswlib::Metric::COSINE);

        std::vector<folly::Future<folly::Unit>> futs;
        futs.reserve(nq);
        for (int i = 0; i < nq; ++i) {
            futs.emplace_back(search_pool_->push([&, idx = i, p_id_ptr = p_id.get(), p_dist_ptr = p_dist.get()]() {
                auto single_query = (const char*)xq + idx * index_->data_size_;
                auto rst = index_->searchKnn(single_query, k, bitset, &param, feder_result);
                size_t rst_size = rst.size();
                auto p_single_dis = p_dist_ptr + idx * k;
                auto p_single_id = p_id_ptr + idx * k;
                for (size_t idx = 0; idx < rst_size; ++idx) {
                    const auto& [dist, id] = rst[idx];
                    p_single_dis[idx] = transform ? (-dist) : dist;
                    p_single_id[idx] = id;
                }
                for (size_t idx = rst_size; idx < (size_t)k; idx++) {
                    p_single_dis[idx] = DistType(1.0 / 0.0);
                    p_single_id[idx] = -1;
                }
            }));
        }
        WaitAllSuccess(futs);

        auto res = GenResultDataSet(nq, k, std::move(p_id), std::move(p_dist));

        // set visit_info json string into result dataset
        if (feder_result != nullptr) {
            Json json_visit_info, json_id_set;
            nlohmann::to_json(json_visit_info, feder_result->visit_info_);
            nlohmann::to_json(json_id_set, feder_result->id_set_);
            res->SetJsonInfo(json_visit_info.dump());
            res->SetJsonIdSet(json_id_set.dump());
        }
        return res;
    }

 private:
    class iterator : public IndexIterator {
     public:
        iterator(const hnswlib::HierarchicalNSW<DataType, DistType, quant_type>* index, const char* query,
                 const bool transform, const BitsetView& bitset, const size_t ef = kIteratorSeedEf,
                 const float refine_ratio = 0.5f, bool use_knowhere_search_pool = true)
            : IndexIterator(transform, use_knowhere_search_pool,
                            (hnswlib::HierarchicalNSW<DataType, DistType, quant_type>::sq_enabled &&
                             hnswlib::HierarchicalNSW<DataType, DistType, quant_type>::has_raw_data)
                                ? refine_ratio
                                : 0.0f),
              index_(index),
              transform_(transform),
              workspace_(index_->getIteratorWorkspace(query, ef, bitset)) {
        }

     protected:
        void
        next_batch(std::function<void(const std::vector<DistId>&)> batch_handler) override {
            index_->getIteratorNextBatch(workspace_.get());
            if (transform_) {
                for (auto& p : workspace_->dists) {
                    p.val = -p.val;
                }
            }
            batch_handler(workspace_->dists);
            workspace_->dists.clear();
        }
        float
        raw_distance(int64_t id) override {
            if constexpr (hnswlib::HierarchicalNSW<DataType, DistType, quant_type>::sq_enabled &&
                          hnswlib::HierarchicalNSW<DataType, DistType, quant_type>::has_raw_data) {
                return (transform_ ? -1 : 1) * index_->calcRefineDistance(workspace_->raw_query_data.get(), id);
            }
            throw std::runtime_error("raw_distance not supported: index does not have raw data or sq is not enabled");
        }

     private:
        const hnswlib::HierarchicalNSW<DataType, DistType, quant_type>* index_;
        const bool transform_;
        std::unique_ptr<hnswlib::IteratorWorkspace> workspace_;
    };

 public:
    expected<std::vector<IndexNode::IteratorPtr>>
    AnnIterator(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset,
                bool use_knowhere_search_pool = true) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "creating iterator on empty index";
            return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::empty_index, "index not loaded");
        }
        auto nq = dataset->GetRows();
        auto xq = dataset->GetTensor();

        auto hnsw_cfg = static_cast<const BaseHnswConfig&>(*cfg);
        auto ef = hnsw_cfg.ef.value_or(kIteratorSeedEf);

        bool transform =
            (index_->metric_type_ == hnswlib::Metric::INNER_PRODUCT || index_->metric_type_ == hnswlib::Metric::COSINE);
        auto vec = std::vector<IndexNode::IteratorPtr>(nq, nullptr);
        try {
            for (int i = 0; i < nq; ++i) {
                auto single_query = (const char*)xq + i * index_->data_size_;
                auto it = std::make_shared<iterator>(this->index_, single_query, transform, bitset, ef,
                                                     hnsw_cfg.iterator_refine_ratio.value(), use_knowhere_search_pool);
                vec[i] = it;
            }
        } catch (const std::exception& e) {
            return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::hnsw_inner_error, e.what());
        }

        return vec;
    }

    expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "range search on empty index";
            return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
        }

        auto nq = dataset->GetRows();
        auto xq = dataset->GetTensor();

        auto hnsw_cfg = static_cast<const BaseHnswConfig&>(*cfg);
        bool is_ip =
            (index_->metric_type_ == hnswlib::Metric::INNER_PRODUCT || index_->metric_type_ == hnswlib::Metric::COSINE);
        DistType range_filter = hnsw_cfg.range_filter.value();

        DistType radius_for_calc = (is_ip ? -hnsw_cfg.radius.value() : hnsw_cfg.radius.value());
        DistType radius_for_filter = hnsw_cfg.radius.value();

        feder::hnsw::FederResultUniq feder_result;
        if (hnsw_cfg.trace_visit.value()) {
            if (nq != 1) {
                return expected<DataSetPtr>::Err(Status::invalid_args, "nq must be 1");
            }
            feder_result = std::make_unique<feder::hnsw::FederResult>();
        }

        hnswlib::SearchParam param{(size_t)hnsw_cfg.ef.value()};

        std::vector<std::vector<int64_t>> result_id_array(nq);
        std::vector<std::vector<DistType>> result_dist_array(nq);

        std::vector<folly::Future<folly::Unit>> futs;
        futs.reserve(nq);
        for (int64_t i = 0; i < nq; ++i) {
            futs.emplace_back(search_pool_->push([&, idx = i]() {
                auto single_query = (const char*)xq + idx * index_->data_size_;
                auto rst = index_->searchRange(single_query, radius_for_calc, bitset, &param, feder_result);
                auto elem_cnt = rst.size();
                result_dist_array[idx].resize(elem_cnt);
                result_id_array[idx].resize(elem_cnt);
                for (size_t j = 0; j < elem_cnt; j++) {
                    auto& p = rst[j];
                    result_dist_array[idx][j] = (is_ip ? (-p.first) : p.first);
                    result_id_array[idx][j] = p.second;
                }
                if (hnsw_cfg.range_filter.value() != defaultRangeFilter) {
                    FilterRangeSearchResultForOneNq(result_dist_array[idx], result_id_array[idx], is_ip,
                                                    radius_for_filter, range_filter);
                }
            }));
        }
        WaitAllSuccess(futs);

        // filter range search result
        auto range_search_result =
            GetRangeSearchResult(result_dist_array, result_id_array, is_ip, nq, radius_for_filter, range_filter);

        auto res = GenResultDataSet(nq, std::move(range_search_result));

        // set visit_info json string into result dataset
        if (feder_result != nullptr) {
            Json json_visit_info, json_id_set;
            nlohmann::to_json(json_visit_info, feder_result->visit_info_);
            nlohmann::to_json(json_id_set, feder_result->id_set_);
            res->SetJsonInfo(json_visit_info.dump());
            res->SetJsonIdSet(json_id_set.dump());
        }
        return res;
    }

    expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const override {
        if (!index_) {
            return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
        }

        auto dim = Dim();
        auto rows = dataset->GetRows();
        auto ids = dataset->GetIds();

        try {
            auto data = std::make_unique<uint8_t[]>(index_->data_size_ * rows);
            for (int64_t i = 0; i < rows; i++) {
                int64_t id = ids[i];
                assert(id >= 0 && id < (int64_t)index_->cur_element_count);
                std::copy_n(index_->getDataByInternalId(id), index_->data_size_, data.get() + i * index_->data_size_);
            }
            return GenResultDataSet(rows, dim, std::move(data));
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "hnsw inner error: " << e.what();
            return expected<DataSetPtr>::Err(Status::hnsw_inner_error, e.what());
        }
    }

    static bool
    StaticHasRawData(const knowhere::BaseConfig& /*config*/, const IndexVersion& version) {
        return quant_type == QuantType::None || quant_type == QuantType::SQ8Refine;
    }

    bool
    HasRawData(const std::string& /*metric_type*/) const override {
        return quant_type == QuantType::None || quant_type == QuantType::SQ8Refine;
    }

    expected<DataSetPtr>
    GetIndexMeta(std::unique_ptr<Config> cfg) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "get index meta on empty index";
            return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
        }

        auto hnsw_cfg = static_cast<const BaseHnswConfig&>(*cfg);
        auto overview_levels = hnsw_cfg.overview_levels.value();
        feder::hnsw::HNSWMeta meta(index_->ef_construction_, index_->M_, index_->cur_element_count, index_->maxlevel_,
                                   index_->enterpoint_node_, overview_levels);
        std::unordered_set<int64_t> id_set;

        for (int i = 0; i < overview_levels; i++) {
            int64_t level = index_->maxlevel_ - i;
            // do not record level 0
            if (level <= 0) {
                break;
            }
            meta.AddLevelLinkGraph(level);
            UpdateLevelLinkList(level, meta, id_set);
        }

        Json json_meta, json_id_set;
        nlohmann::to_json(json_meta, meta);
        nlohmann::to_json(json_id_set, id_set);
        return GenResultDataSet(json_meta.dump(), json_id_set.dump());
    }

    Status
    Serialize(BinarySet& binset) const override {
        if (!index_) {
            LOG_KNOWHERE_ERROR_ << "Can not serialize empty HNSW index.";
            return Status::empty_index;
        }
        try {
            MemoryIOWriter writer;
            index_->saveIndex(writer);
            std::shared_ptr<uint8_t[]> data(writer.data());
            binset.Append(Type(), data, writer.tellg());
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "hnsw inner error: " << e.what();
            return Status::hnsw_inner_error;
        }
        return Status::success;
    }

    Status
    Deserialize(BinarySet&& binset, std::shared_ptr<Config>) override {
        if (index_) {
            delete index_;
        }
        try {
            binarySet_ = std::move(binset);
            auto binary = binarySet_.GetByName(Type());
            if (binary == nullptr) {
                LOG_KNOWHERE_ERROR_ << "Invalid binary set.";
                return Status::invalid_binary_set;
            }

            ZeroCopyIOReader reader(binary->data.get(), binary->size);

            hnswlib::SpaceInterface<DistType>* space = nullptr;
            index_ = new (std::nothrow) hnswlib::HierarchicalNSW<DataType, DistType, quant_type>(space);
            index_->loadIndex(reader);
            LOG_KNOWHERE_INFO_ << "Loaded HNSW index. #points num:" << index_->max_elements_ << " #M:" << index_->M_
                               << " #max level:" << index_->maxlevel_
                               << " #ef_construction:" << index_->ef_construction_
                               << " #dim:" << *(size_t*)(index_->space_->get_dist_func_param());
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "hnsw inner error: " << e.what();
            return Status::hnsw_inner_error;
        }
        return Status::success;
    }

    Status
    DeserializeFromFile(const std::string& filename, std::shared_ptr<Config> cfg) override {
        if (index_) {
            delete index_;
        }
        try {
            hnswlib::SpaceInterface<DistType>* space = nullptr;
            index_ = new (std::nothrow) hnswlib::HierarchicalNSW<DataType, DistType, quant_type>(space);
            index_->loadIndex(filename, *cfg);
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "hnsw inner error: " << e.what();
            return Status::hnsw_inner_error;
        }
        return Status::success;
    }

    static std::unique_ptr<BaseConfig>
    StaticCreateConfig() {
        return std::make_unique<HnswConfig>();
    }

    std::unique_ptr<BaseConfig>
    CreateConfig() const override {
        return StaticCreateConfig();
    }

    int64_t
    Dim() const override {
        if (!index_) {
            return 0;
        }
        return (*static_cast<size_t*>(index_->dist_func_param_));
    }

    int64_t
    Size() const override {
        if (!index_) {
            return 0;
        }
        return index_->cal_size();
    }

    int64_t
    Count() const override {
        if (!index_) {
            return 0;
        }
        return index_->cur_element_count;
    }

    std::string
    Type() const override {
        return knowhere::IndexEnum::INDEX_HNSW;
    }

    ~HnswIndexNode() override {
        if (index_) {
            delete index_;
        }
    }

 private:
    void
    UpdateLevelLinkList(int32_t level, feder::hnsw::HNSWMeta& meta, std::unordered_set<int64_t>& id_set) const {
        if (!(level > 0 && level <= index_->maxlevel_)) {
            return;
        }
        if (index_->cur_element_count == 0) {
            return;
        }

        std::vector<hnswlib::tableint> level_elements;

        // get all elements in current level
        for (size_t i = 0; i < index_->cur_element_count; i++) {
            // elements in high level also exist in low level
            if (index_->element_levels_[i] >= level) {
                level_elements.emplace_back(i);
            }
        }

        // iterate all elements in current level, record their link lists
        for (auto curr_id : level_elements) {
            auto data = index_->get_linklist(curr_id, level);
            auto size = index_->getListCount(data);

            hnswlib::tableint* datal = (hnswlib::tableint*)(data + 1);
            std::vector<int64_t> neighbors(size);
            for (int i = 0; i < size; i++) {
                hnswlib::tableint cand = datal[i];
                neighbors[i] = cand;
            }
            id_set.insert(curr_id);
            id_set.insert(neighbors.begin(), neighbors.end());
            meta.AddNodeInfo(level, curr_id, std::move(neighbors));
        }
    }

 private:
    hnswlib::HierarchicalNSW<DataType, DistType, quant_type>* index_;
    std::shared_ptr<ThreadPool> search_pool_;
};

}  // namespace knowhere
