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
#ifndef INDEX_NODE_WITH_DATA_VIEW_REFINER_H
#define INDEX_NODE_WITH_DATA_VIEW_REFINER_H
#include <atomic>
#include <random>

#include "faiss/utils/random.h"
#include "index/data_view_dense_index/data_view_dense_index.h"
#include "index/data_view_dense_index/data_view_index_config.h"
#include "knowhere/comp/rw_lock.h"
#include "knowhere/index/index_node.h"
namespace knowhere {
struct DataViewIndexFlat;
/*
IndexNodeWithDataViewRefiner is a just in time index, support concurrent build and search.
This kind of index will not keep raw data anymore, so init it with a get raw data function(ViewDataOp).
And it maintain a basic index (code size < raw data size) and a refiner.
If metric == Cosine, base index will normalize all vectors, and replaced with Inner product;
refine_index will compute the IP distances, and divide by ||x|| and ||y||.

todo: basic index use fp32, we should support more type later.
*/
template <typename DataType, typename BaseIndexNode>
class IndexNodeWithDataViewRefiner : public IndexNode {
    static_assert(KnowhereFloatTypeCheck<DataType>::value);

 public:
    IndexNodeWithDataViewRefiner(const int32_t& version, const Object& object) {
        auto data_view_index_pack = dynamic_cast<const Pack<ViewDataOp>*>(&object);
        assert(data_view_index_pack != nullptr);
        view_data_op_ = data_view_index_pack->GetPack();
        base_index_ = std::make_unique<BaseIndexNode>(version, nullptr);
        base_index_lock_ = std::make_unique<FairRWLock>();
    }

    Status
    Train(const DataSetPtr dataset, std::shared_ptr<Config> cfg) override;

    Status
    Add(const DataSetPtr dataset, std::shared_ptr<Config> cfg) override;

    expected<DataSetPtr>
    Search(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override;

    expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override;

    expected<std::vector<IndexNode::IteratorPtr>>
    AnnIterator(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset,
                bool use_knowhere_search_pool) const override;

    expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const override {
        return expected<DataSetPtr>::Err(Status::not_implemented, "Data View Index not maintain raw data.");
    }

    static Status
    StaticConfigCheck(const Config& cfg, PARAM_TYPE paramType, std::string& msg) {
        auto base_cfg = static_cast<const BaseConfig&>(cfg);
        if constexpr (KnowhereFloatTypeCheck<DataType>::value) {
            if (IsMetricType(base_cfg.metric_type.value(), metric::L2) ||
                IsMetricType(base_cfg.metric_type.value(), metric::IP) ||
                IsMetricType(base_cfg.metric_type.value(), metric::COSINE)) {
            } else {
                msg = "metric type " + base_cfg.metric_type.value() +
                      " not found or not supported, supported: [L2 IP COSINE]";
                return Status::invalid_metric_type;
            }
        }
        return Status::success;
    }

    static bool
    CommonHasRawData() {
        return false;
    }

    static bool
    StaticHasRawData(const knowhere::BaseConfig& config, const IndexVersion& version) {
        return false;
    }

    bool
    HasRawData(const std::string& metric_type) const override {
        return false;
    }

    expected<DataSetPtr>
    GetIndexMeta(std::unique_ptr<Config> cfg) const override {
        if (!this->base_index_) {
            return expected<DataSetPtr>::Err(Status::empty_index, "Data View Index not maintain raw data.");
        }
        FairReadLockGuard(*this->base_index_lock_);
        auto meta = this->base_index_->GetIndexMeta(std::move(cfg));
        return meta;
    }

    Status
    Serialize(BinarySet& binset) const override {
        LOG_KNOWHERE_ERROR_ << "Data View Index is parasitic on the raw data structure, do not Serialize.";
        return Status::not_implemented;
    }

    Status
    Deserialize(const BinarySet& binset, std::shared_ptr<Config> cfg) override {
        LOG_KNOWHERE_ERROR_ << "Data View Index is parasitic on the raw data structure, do not Deserialize";
        return Status::not_implemented;
    }

    Status
    DeserializeFromFile(const std::string& filename, std::shared_ptr<Config> cfg) override {
        LOG_KNOWHERE_ERROR_ << "Data View Index is parasitic on the raw data structure, do not DeserializeFromFile";
        return Status::not_implemented;
    }

    static std::unique_ptr<BaseConfig>
    StaticCreateConfig() {
        auto base_index_cfg = BaseIndexNode::StaticCreateConfig();
        if (dynamic_cast<ScannConfig*>(base_index_cfg.get())) {
            return std::make_unique<ScannWithDataViewRefinerConfig>();
        } else {
            return std::make_unique<IndexWithDataViewRefinerConfig>();
        }
    }

    std::unique_ptr<BaseConfig>
    CreateConfig() const override {
        if (base_index_->Type() == IndexEnum::INDEX_FAISS_SCANN) {
            return std::make_unique<ScannWithDataViewRefinerConfig>();
        } else {
            return std::make_unique<IndexWithDataViewRefinerConfig>();
        }
    }

    int64_t
    Dim() const override {
        if (!this->refine_offset_index_) {
            return -1;
        }
        return refine_offset_index_->Dim();
    }

    int64_t
    Size() const override {
        if (this->base_index_) {
            FairReadLockGuard(*this->base_index_lock_);
            auto size = this->base_index_->Size();
            return size;
        }
        return 0;
    }

    int64_t
    Count() const override {
        if (this->base_index_) {
            FairReadLockGuard(*this->base_index_lock_);
            auto count = this->base_index_->Count();
            return count;
        }
        return 0;
    }

    std::string
    Type() const override;

 private:
    class iterator : public IndexIterator {
     public:
        iterator(std::shared_ptr<DataViewIndexFlat> refine_offset_index, IndexNode::IteratorPtr base_workspace,
                 std::unique_ptr<DataType[]>&& copied_query, bool larger_is_closer, float refine_ratio = 0.5f,
                 bool retain_iterator_order = false)
            : IndexIterator(larger_is_closer, false, refine_ratio, retain_iterator_order),
              refine_offset_index_(refine_offset_index),
              copied_query_(std::move(copied_query)),
              base_workspace_(base_workspace) {
            refine_computer_ = SelectDataViewComputer(refine_offset_index.get());
            refine_computer_->set_query((const float*)copied_query_.get());
        }

        std::pair<int64_t, float>
        Next() override {
            if (!initialized_) {
                initialize();
            }
            if (!refine_) {
                return base_workspace_->Next();
            } else {
                auto ret = refined_res_.top();
                refined_res_.pop();
                UpdateNext();
                if (retain_iterator_order_) {
                    while (HasNext()) {
                        auto next_ret = refined_res_.top();
                        if (next_ret.val >= ret.val) {
                            break;
                        }
                        refined_res_.pop();
                        UpdateNext();
                    }
                }
                return std::make_pair(ret.id, ret.val * sign_);
            }
        }

        [[nodiscard]] bool
        HasNext() override {
            if (!initialized_) {
                initialize();
            }
            if (!refine_) {
                return base_workspace_->HasNext();
            } else {
                return !refined_res_.empty() || base_workspace_->HasNext();
            }
        }

        void
        initialize() override {
            if (initialized_) {
                throw std::runtime_error("initialize should not be called twice");
            }
            UpdateNext();
            initialized_ = true;
        }

     protected:
        float
        raw_distance(int64_t id) override {
            if (refine_computer_ == nullptr) {
                throw std::runtime_error("refine computer is null in offset refine index.");
            }
            if (refine_offset_index_->Count() <= id) {
                throw std::runtime_error("the id of result larger than index rows count.");
            }
            float dis = refine_computer_->operator()(id);
            dis = refine_offset_index_->IsCosine() ? dis / refine_offset_index_->GetDataNorm(id) : dis;
            return dis;
        }

     private:
        void
        UpdateNext() {
            if (!base_workspace_->HasNext() || refine_ == false) {
                return;
            }
            while (base_workspace_->HasNext() && (refined_res_.empty() || refined_res_.size() < min_refine_size())) {
                auto pair = base_workspace_->Next();
                refined_res_.emplace(pair.first, raw_distance(pair.first) * sign_);
            }
        }

     private:
        bool initialized_ = false;
        std::shared_ptr<DataViewIndexFlat> refine_offset_index_ = nullptr;
        std::unique_ptr<DataType[]> copied_query_ = nullptr;
        IndexNode::IteratorPtr base_workspace_ = nullptr;
        std::unique_ptr<faiss::DistanceComputer> refine_computer_ = nullptr;
    };
    bool is_cosine_;
    ViewDataOp view_data_op_;
    std::shared_ptr<DataViewIndexFlat>
        refine_offset_index_;                // a data view flat index to maintain raw data without extra memory
    std::unique_ptr<IndexNode> base_index_;  // base_index will hold data codes in memory, datatype is fp32
    std::unique_ptr<FairRWLock>
        base_index_lock_;  // base_index_lock_ protect all concurrent writes/reads access of base_index_
};

namespace {
constexpr int64_t kBatchSize = 4096;
constexpr int64_t kMaxTrainSize = 5000;
constexpr int64_t kRandomSeed = 1234;
constexpr const char* kIndexNodeSuffixWithDataViewRefiner = "_DVR";  // short express of Data View Refine

template <typename DataType>
inline DataSetPtr
GenBaseIndexFp32TrainDataSet(const DataSetPtr& src, bool is_cosine = false,
                             std::optional<size_t> filling_dim = std::nullopt) {
    DataSetPtr train_ds;
    auto rows = src->GetRows();
    bool cosine_need_copy = false;
    size_t src_dim = src->GetDim();
    size_t des_dim = filling_dim.value_or(src_dim);
    assert(src_dim <= des_dim);

    if (rows <= kMaxTrainSize && des_dim == src_dim) {
        train_ds = ConvertFromDataTypeIfNeeded<DataType>(src);
        if constexpr (std::is_same_v<DataType, fp32>) {
            cosine_need_copy = true;
        }
    } else {
        auto train_rows = std::min(rows, kMaxTrainSize);
        std::vector<int> random_ids(rows);
        faiss::rand_perm(random_ids.data(), rows, kRandomSeed);

        const DataType* src_data = (const DataType*)src->GetTensor();
        auto* des_data = new float[des_dim * train_rows];
        std::memset(des_data, 0, sizeof(float) * des_dim * train_rows);
        for (auto i = 0; i < train_rows; i++) {
            auto from_id = random_ids[i];
            auto to_id = i;
            if constexpr (std::is_same_v<DataType, knowhere::fp32>) {
                std::memcpy(des_data + to_id * des_dim, src_data + from_id * src_dim, sizeof(float) * src_dim);
            } else {
                for (size_t d = 0; d < src_dim; d++) {
                    // todo: optimize it with simd
                    des_data[to_id * des_dim + d] = (fp32)src_data[from_id * src_dim + d];
                }
            }
        }
        auto des = std::make_shared<DataSet>();
        des->SetRows(train_rows);
        des->SetDim(des_dim);
        des->SetTensor(des_data);
        des->SetIsOwner(true);
        train_ds = des;
    }
    if (is_cosine) {
        if (cosine_need_copy) {
            train_ds = std::get<0>(CopyAndNormalizeDataset<fp32>(train_ds));
        } else {
            NormalizeDataset<fp32>(train_ds);
        }
    }
    return train_ds;
}

template <typename DataType>
inline std::tuple<DataSetPtr, std::vector<float>>
ConvertToBaseIndexFp32DataSet(const DataSetPtr& src, bool is_cosine = false,
                              const std::optional<int64_t> start = std::nullopt,
                              const std::optional<int64_t> count = std::nullopt,
                              const std::optional<int64_t> filling_dim = std::nullopt) {
    auto src_dim = src->GetDim();
    auto des_dim = filling_dim.value_or(src_dim);
    auto fp32_ds = ConvertFromDataTypeIfNeeded<DataType>(src, start, count, filling_dim);
    if (is_cosine) {
        if (std::is_same_v<DataType, fp32> && src_dim == des_dim) {
            return CopyAndNormalizeDataset<fp32>(fp32_ds);
        } else {
            auto rows = fp32_ds->GetRows();
            auto norms_vec = NormalizeVecs((float*)fp32_ds->GetTensor(), rows, des_dim);
            return std::make_tuple(fp32_ds, norms_vec);
        }
    }
    return std::make_tuple(fp32_ds, std::vector<float>());
}
}  // namespace

template <typename DataType, typename BaseIndexNode>
Status
IndexNodeWithDataViewRefiner<DataType, BaseIndexNode>::Train(const DataSetPtr dataset, std::shared_ptr<Config> cfg) {
    BaseConfig& base_cfg = static_cast<BaseConfig&>(*cfg);
    this->is_cosine_ = IsMetricType(base_cfg.metric_type.value(), knowhere::metric::COSINE);
    auto dim = dataset->GetDim();

    // construct refiner
    auto refine_metric = is_cosine_ ? metric::IP : base_cfg.metric_type.value();
    refine_offset_index_ =
        std::make_unique<DataViewIndexFlat>(dim, datatype_v<DataType>, refine_metric, this->view_data_op_, is_cosine_);
    // construct quant index and train:
    AdaptToBaseIndexConfig(cfg.get(), PARAM_TYPE::TRAIN, dim);
    auto base_index_dim = dynamic_cast<BaseConfig*>(cfg.get())->dim.value();

    LOG_KNOWHERE_DEBUG_ << "Generate Base Index with dim: " << base_index_dim << std::endl;
    auto fp32_train_ds = GenBaseIndexFp32TrainDataSet<DataType>(dataset, this->is_cosine_, base_index_dim);
    return base_index_->Train(
        fp32_train_ds,
        cfg);  // train not need base_index_lock_, all add and search will fail if train not called before
}

template <typename DataType, typename BaseIndexNode>
Status
IndexNodeWithDataViewRefiner<DataType, BaseIndexNode>::Add(const DataSetPtr dataset, std::shared_ptr<Config> cfg) {
    auto rows = dataset->GetRows();
    auto dim = dataset->GetDim();
    AdaptToBaseIndexConfig(cfg.get(), PARAM_TYPE::TRAIN, dim);
    Status add_stat;
    for (auto blk_i = 0; blk_i < rows; blk_i += kBatchSize) {
        auto blk_size = std::min(kBatchSize, rows - blk_i);
        auto [base_ds, norms] =
            ConvertToBaseIndexFp32DataSet<DataType>(dataset, is_cosine_, blk_i, blk_size, base_index_->Dim());
        {
            FairWriteLockGuard(*this->base_index_lock_);
            add_stat = base_index_->Add(base_ds, cfg);
        }
        try {
            refine_offset_index_->Add(blk_size, nullptr, norms.data());
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "data view index inner error: " << e.what();
            return Status::internal_error;
        }

        if (add_stat != Status::success) {
            return add_stat;
        }
    }
    return Status::success;
}

template <typename DataType, typename BaseIndexNode>
expected<DataSetPtr>
IndexNodeWithDataViewRefiner<DataType, BaseIndexNode>::Search(const DataSetPtr dataset, std::unique_ptr<Config> cfg,
                                                              const BitsetView& bitset) const {
    if (this->base_index_ == nullptr || this->refine_offset_index_ == nullptr) {
        LOG_KNOWHERE_WARNING_ << "search on empty index";
        return expected<DataSetPtr>::Err(Status::empty_index, "index not is trained.");
    }
    BaseConfig& base_cfg = static_cast<IndexWithDataViewRefinerConfig&>(*cfg);
    auto nq = dataset->GetRows();
    auto dim = dataset->GetDim();
    auto topk = base_cfg.k.value();
    // basic search
    AdaptToBaseIndexConfig(cfg.get(), PARAM_TYPE::SEARCH, dim);
    auto base_index_ds = std::get<0>(
        ConvertToBaseIndexFp32DataSet<DataType>(dataset, is_cosine_, std::nullopt, std::nullopt, base_index_->Dim()));
    knowhere::expected<knowhere::DataSetPtr> quant_res;
    {
        FairReadLockGuard(*this->base_index_lock_);
        quant_res = base_index_->Search(base_index_ds, std::move(cfg), bitset);
    }
    if (!quant_res.has_value()) {
        return quant_res;
    }
    // refine
    auto queries_lims = std::vector<idx_t>(nq + 1);
    auto reorder_k = quant_res.value()->GetDim();
    for (auto i = 0; i < nq + 1; i++) {
        queries_lims[i] = reorder_k * i;
    }
    auto refine_ids = quant_res.value()->GetIds();
    auto labels = std::make_unique<int64_t[]>(nq * topk);
    auto distances = std::make_unique<float[]>(nq * topk);
    try {
        refine_offset_index_->SearchWithIds(nq, dataset->GetTensor(), queries_lims.data(), refine_ids, topk,
                                            distances.get(), labels.get());
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "data view index inner error: " << e.what();
        return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
    }
    return GenResultDataSet(nq, topk, std::move(labels), std::move(distances));
}

template <typename DataType, typename BaseIndexNode>
expected<DataSetPtr>
IndexNodeWithDataViewRefiner<DataType, BaseIndexNode>::RangeSearch(const DataSetPtr dataset,
                                                                   std::unique_ptr<Config> cfg,
                                                                   const BitsetView& bitset) const {
    if (this->base_index_ == nullptr || this->refine_offset_index_ == nullptr) {
        LOG_KNOWHERE_WARNING_ << "search on empty index";
        return expected<DataSetPtr>::Err(Status::empty_index, "index not is trained.");
    }
    const BaseConfig& base_cfg = static_cast<const BaseConfig&>(*cfg);
    auto nq = dataset->GetRows();
    auto dim = dataset->GetDim();
    auto radius = base_cfg.radius.value();
    auto range_filter = base_cfg.range_filter.value();
    AdaptToBaseIndexConfig(cfg.get(), PARAM_TYPE::RANGE_SEARCH, dim);
    auto base_index_ds = std::get<0>(
        ConvertToBaseIndexFp32DataSet<DataType>(dataset, is_cosine_, std::nullopt, std::nullopt, base_index_->Dim()));

    knowhere::expected<knowhere::DataSetPtr> quant_res;
    {
        FairReadLockGuard(*this->base_index_lock_);
        quant_res = base_index_->RangeSearch(base_index_ds, std::move(cfg), bitset);
    }
    if (!quant_res.has_value()) {
        return quant_res;
    }
    auto quant_res_ids = quant_res.value()->GetIds();
    auto quant_res_lims = quant_res.value()->GetLims();
    try {
        auto final_res =
            refine_offset_index_->RangeSearchWithIds(nq, dataset->GetTensor(), (const knowhere::idx_t*)quant_res_lims,
                                                     (const knowhere::idx_t*)quant_res_ids, radius, range_filter);
        return GenResultDataSet(nq, std::move(final_res));
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "data view index inner error: " << e.what();
        return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
    }
}

template <typename DataType, typename BaseIndexNode>
expected<std::vector<IndexNode::IteratorPtr>>
IndexNodeWithDataViewRefiner<DataType, BaseIndexNode>::AnnIterator(const DataSetPtr dataset,
                                                                   std::unique_ptr<Config> cfg,
                                                                   const BitsetView& bitset,
                                                                   bool use_knowhere_search_pool) const {
    if (this->base_index_ == nullptr || this->refine_offset_index_ == nullptr) {
        LOG_KNOWHERE_WARNING_ << "search on empty index";
        return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::empty_index, "index not is trained.");
    }

    auto dim = dataset->GetDim();
    auto nq = dataset->GetRows();
    auto data = dataset->GetTensor();
    AdaptToBaseIndexConfig(cfg.get(), PARAM_TYPE::ITERATOR, dim);
    const auto& base_cfg = static_cast<const BaseConfig&>(*cfg);
    auto refine_ratio = base_cfg.iterator_refine_ratio.value();
    auto larger_is_closer = IsMetricType(base_cfg.metric_type.value(), knowhere::metric::IP) || is_cosine_;
    auto base_index_ds = std::get<0>(
        ConvertToBaseIndexFp32DataSet<DataType>(dataset, is_cosine_, std::nullopt, std::nullopt, base_index_->Dim()));
    knowhere::expected<std::vector<knowhere::IndexNode::IteratorPtr>> base_index_init;
    {
        FairReadLockGuard(*this->base_index_lock_);
        base_index_init = base_index_->AnnIterator(base_index_ds, std::move(cfg), bitset, use_knowhere_search_pool);
    }
    if (!base_index_init.has_value()) {
        return base_index_init;
    }
    auto base_workspace_iters = base_index_init.value();
    if (base_workspace_iters.size() != (size_t)nq) {
        return expected<std::vector<IndexNode::IteratorPtr>>::Err(
            Status::internal_error, "quant workspace is not equal to the rows count of input dataset.");
    }
    auto vec = std::vector<IndexNode::IteratorPtr>(nq, nullptr);
    for (auto i = 0; i < nq; i++) {
        auto cur_query = (const DataType*)data + i * dim;
        std::unique_ptr<DataType[]> copied_query = nullptr;
        copied_query = std::make_unique<DataType[]>(dim);
        std::copy_n(cur_query, dim, copied_query.get());
        vec[i] = std::shared_ptr<iterator>(new iterator(this->refine_offset_index_, base_workspace_iters[i],
                                                        std::move(copied_query), larger_is_closer, refine_ratio));
    }
    return vec;
}

template <typename DataType, typename BaseIndexNode>
std::string
IndexNodeWithDataViewRefiner<DataType, BaseIndexNode>::Type() const {
    return base_index_->Type() + kIndexNodeSuffixWithDataViewRefiner;
}

}  // namespace knowhere
#endif
