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

#include "common/metric.h"
#include "faiss/IndexBinaryFlat.h"
#include "faiss/IndexFlat.h"
#include "faiss/impl/AuxIndexStructures.h"
#include "faiss/impl/zerocopy_io.h"
#include "faiss/index_io.h"
#include "index/flat/flat_config.h"
#include "io/memory_io.h"
#include "knowhere/bitsetview_idselector.h"
#include "knowhere/comp/thread_pool.h"
#include "knowhere/feature.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/index/index_node_data_mock_wrapper.h"
#include "knowhere/log.h"
#include "knowhere/range_util.h"
#include "knowhere/utils.h"

namespace knowhere {

template <typename DataType, typename IndexType>
class FlatIndexNode : public IndexNode {
 public:
    FlatIndexNode(const int32_t version, const Object& object) : IndexNode(version), index_(nullptr) {
        static_assert(
            std::is_same<IndexType, faiss::IndexFlat>::value || std::is_same<IndexType, faiss::IndexBinaryFlat>::value,
            "not support");
        static_assert(std::is_same_v<DataType, fp32> || std::is_same_v<DataType, bin1>,
                      "FlatIndexNode only support float/binary");
        search_pool_ = ThreadPool::GetGlobalSearchThreadPool();
    }

    Status
    Train(const DataSetPtr dataset, std::shared_ptr<Config> cfg) override {
        const FlatConfig& f_cfg = static_cast<const FlatConfig&>(*cfg);

        auto metric = Str2FaissMetricType(f_cfg.metric_type.value());
        if (!metric.has_value()) {
            LOG_KNOWHERE_ERROR_ << "unsupported metric type: " << f_cfg.metric_type.value();
            return metric.error();
        }
        if constexpr (std::is_same<faiss::IndexBinaryFlat, IndexType>::value) {
            index_ = std::make_unique<faiss::IndexBinaryFlat>(dataset->GetDim(), metric.value());
        }
        if constexpr (std::is_same<faiss::IndexFlat, IndexType>::value) {
            bool is_cosine = IsMetricType(f_cfg.metric_type.value(), knowhere::metric::COSINE);
            index_ = std::make_unique<faiss::IndexFlat>(dataset->GetDim(), metric.value(), is_cosine);
        }
        return Status::success;
    }

    Status
    Add(const DataSetPtr dataset, std::shared_ptr<Config>) override {
        auto x = dataset->GetTensor();
        auto n = dataset->GetRows();
        index_->add(n, (const DataType*)x);
        return Status::success;
    }

    expected<DataSetPtr>
    Search(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "search on empty index";
            return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
        }

        DataSetPtr results = std::make_shared<DataSet>();
        const FlatConfig& f_cfg = static_cast<const FlatConfig&>(*cfg);
        bool is_cosine = IsMetricType(f_cfg.metric_type.value(), knowhere::metric::COSINE);

        auto k = f_cfg.k.value();
        auto nq = dataset->GetRows();
        auto x = dataset->GetTensor();
        auto dim = dataset->GetDim();

        auto len = k * nq;
        int64_t* ids = nullptr;
        float* distances = nullptr;
        try {
            ids = new (std::nothrow) int64_t[len];
            distances = new (std::nothrow) float[len];
            std::vector<folly::Future<folly::Unit>> futs;
            futs.reserve(nq);
            for (int i = 0; i < nq; ++i) {
                futs.emplace_back(search_pool_->push([&, index = i] {
                    ThreadPool::ScopedSearchOmpSetter setter(1);
                    auto cur_ids = ids + k * index;
                    auto cur_dis = distances + k * index;

                    BitsetViewIDSelector bw_idselector(bitset);
                    faiss::IDSelector* id_selector = (bitset.empty()) ? nullptr : &bw_idselector;

                    if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
                        auto cur_query = (const DataType*)x + dim * index;
                        std::unique_ptr<DataType[]> copied_query = nullptr;
                        if (is_cosine) {
                            copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                            cur_query = copied_query.get();
                        }

                        faiss::SearchParameters search_params;
                        search_params.sel = id_selector;

                        index_->search(1, cur_query, k, cur_dis, cur_ids, &search_params);
                    }
                    if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
                        auto cur_i_dis = reinterpret_cast<int32_t*>(cur_dis);

                        faiss::SearchParameters search_params;
                        search_params.sel = id_selector;

                        index_->search(1, (const uint8_t*)x + index * ((dim + 7) / 8), k, cur_i_dis, cur_ids,
                                       &search_params);

                        if (index_->metric_type == faiss::METRIC_Hamming) {
                            for (int64_t j = 0; j < k; j++) {
                                cur_dis[j] = static_cast<float>(cur_i_dis[j]);
                            }
                        }
                    }
                }));
            }
            // wait for the completion
            WaitAllSuccess(futs);
        } catch (const std::exception& e) {
            std::unique_ptr<int64_t[]> auto_delete_ids(ids);
            std::unique_ptr<float[]> auto_delete_dis(distances);
            LOG_KNOWHERE_WARNING_ << "error inner faiss: " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }
        return GenResultDataSet(nq, k, ids, distances);
    }

    expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "range search on empty index";
            return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
        }

        const FlatConfig& f_cfg = static_cast<const FlatConfig&>(*cfg);
        bool is_cosine = IsMetricType(f_cfg.metric_type.value(), knowhere::metric::COSINE);

        auto nq = dataset->GetRows();
        auto xq = dataset->GetTensor();
        auto dim = dataset->GetDim();

        float radius = f_cfg.radius.value();
        float range_filter = f_cfg.range_filter.value();
        bool is_ip = (index_->metric_type == faiss::METRIC_INNER_PRODUCT);

        RangeSearchResult range_search_result;

        std::vector<std::vector<int64_t>> result_id_array(nq);
        std::vector<std::vector<float>> result_dist_array(nq);

        try {
            std::vector<folly::Future<folly::Unit>> futs;
            futs.reserve(nq);
            for (int i = 0; i < nq; ++i) {
                futs.emplace_back(search_pool_->push([&, index = i] {
                    ThreadPool::ScopedSearchOmpSetter setter(1);
                    faiss::RangeSearchResult res(1);

                    BitsetViewIDSelector bw_idselector(bitset);
                    faiss::IDSelector* id_selector = (bitset.empty()) ? nullptr : &bw_idselector;

                    if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
                        auto cur_query = (const DataType*)xq + dim * index;
                        std::unique_ptr<DataType[]> copied_query = nullptr;
                        if (is_cosine) {
                            copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                            cur_query = copied_query.get();
                        }

                        faiss::SearchParameters search_params;
                        search_params.sel = id_selector;

                        index_->range_search(1, cur_query, radius, &res, &search_params);
                    }
                    if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
                        faiss::SearchParameters search_params;
                        search_params.sel = id_selector;

                        index_->range_search(1, (const uint8_t*)xq + index * ((dim + 7) / 8), radius, &res,
                                             &search_params);
                    }
                    auto elem_cnt = res.lims[1];
                    result_dist_array[index].resize(elem_cnt);
                    result_id_array[index].resize(elem_cnt);
                    for (size_t j = 0; j < elem_cnt; j++) {
                        result_dist_array[index][j] = res.distances[j];
                        result_id_array[index][j] = res.labels[j];
                    }
                    if (f_cfg.range_filter.value() != defaultRangeFilter) {
                        FilterRangeSearchResultForOneNq(result_dist_array[index], result_id_array[index], is_ip, radius,
                                                        range_filter);
                    }
                }));
            }
            // wait for the completion
            WaitAllSuccess(futs);
            range_search_result =
                GetRangeSearchResult(result_dist_array, result_id_array, is_ip, nq, radius, range_filter);
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "error inner faiss: " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }

        return GenResultDataSet(nq, std::move(range_search_result));
    }

    expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const override {
        auto dim = Dim();
        auto rows = dataset->GetRows();
        auto ids = dataset->GetIds();
        if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
            DataType* data = nullptr;
            try {
                data = new DataType[rows * dim];
                for (int64_t i = 0; i < rows; i++) {
                    index_->reconstruct(ids[i], data + i * dim);
                }
                return GenResultDataSet(rows, dim, data);
            } catch (const std::exception& e) {
                std::unique_ptr<DataType[]> auto_del(data);
                LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
                return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
            }
        }
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
            uint8_t* data = nullptr;
            try {
                data = new uint8_t[rows * ((dim + 7) / 8)];
                for (int64_t i = 0; i < rows; i++) {
                    index_->reconstruct(ids[i], data + i * ((dim + 7) / 8));
                }
                return GenResultDataSet(rows, dim, data);
            } catch (const std::exception& e) {
                std::unique_ptr<uint8_t[]> auto_del(data);
                LOG_KNOWHERE_WARNING_ << "error inner faiss: " << e.what();
                return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
            }
        }
    }

    static bool
    StaticHasRawData(const knowhere::BaseConfig& config, const IndexVersion& version) {
        if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
            const FlatConfig& f_cfg = static_cast<const FlatConfig&>(config);
            if (knowhere::Version(version) <= Version::GetMinimalVersion()) {
                return !IsMetricType(f_cfg.metric_type.value(), metric::COSINE);
            } else {
                return true;
            }
        }

        if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
            return true;
        }
    }

    bool
    HasRawData(const std::string& metric_type) const override {
        if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
            if (this->version_ <= Version::GetMinimalVersion()) {
                return !IsMetricType(metric_type, metric::COSINE);
            } else {
                return true;
            }
        }
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
            return true;
        }
    }

    expected<DataSetPtr>
    GetIndexMeta(std::unique_ptr<Config>) const override {
        return expected<DataSetPtr>::Err(Status::not_implemented, "GetIndexMeta not implemented");
    }

    Status
    Serialize(BinarySet& binset) const override {
        if (!index_) {
            LOG_KNOWHERE_ERROR_ << "Can not serialize empty index.";
            return Status::empty_index;
        }
        try {
            MemoryIOWriter writer;
            if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
                faiss::write_index(index_.get(), &writer);
            }
            if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
                faiss::write_index_binary(index_.get(), &writer);
            }
            std::shared_ptr<uint8_t[]> data(writer.data());
            binset.Append(Type(), data, writer.tellg());
            return Status::success;
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "error inner faiss: " << e.what();
            return Status::faiss_inner_error;
        }
    }

    Status
    Deserialize(BinarySet&& binset, std::shared_ptr<Config>) override {
        std::vector<std::string> names = {"IVF",        // compatible with knowhere-1.x
                                          "BinaryIVF",  // compatible with knowhere-1.x
                                          Type()};
        binarySet_ = std::move(binset);
        auto binary = binarySet_.GetByNames(names);
        if (binary == nullptr) {
            LOG_KNOWHERE_ERROR_ << "Invalid binary set.";
            return Status::invalid_binary_set;
        }

        int io_flags = faiss::IO_FLAG_ZERO_COPY;
        faiss::ZeroCopyIOReader reader(binary->data.get(), binary->size);
        if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
            faiss::Index* index = faiss::read_index(&reader, io_flags);
            index_.reset(static_cast<IndexType*>(index));
        }
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
            faiss::IndexBinary* index = faiss::read_index_binary(&reader, io_flags);
            index_.reset(static_cast<IndexType*>(index));
        }
        return Status::success;
    }

    Status
    DeserializeFromFile(const std::string& filename, std::shared_ptr<Config> cfg) override {
        auto flat_cfg = static_cast<const knowhere::BaseConfig&>(*cfg);

        int io_flags = 0;
        if (flat_cfg.enable_mmap.value()) {
            io_flags |= faiss::IO_FLAG_MMAP_IFC;
        }

        if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
            faiss::Index* index = faiss::read_index(filename.data(), io_flags);
            index_.reset(static_cast<IndexType*>(index));
        }
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
            faiss::IndexBinary* index = faiss::read_index_binary(filename.data(), io_flags);
            index_.reset(static_cast<IndexType*>(index));
        }
        return Status::success;
    }

    static std::unique_ptr<BaseConfig>
    StaticCreateConfig() {
        return std::make_unique<FlatConfig>();
    }

    std::unique_ptr<BaseConfig>
    CreateConfig() const override {
        return StaticCreateConfig();
    }

    int64_t
    Dim() const override {
        return index_->d;
    }

    int64_t
    Size() const override {
        return index_->ntotal * index_->d * sizeof(DataType);
    }

    int64_t
    Count() const override {
        return index_->ntotal;
    }

    std::string
    Type() const override {
        if constexpr (std::is_same<IndexType, faiss::IndexFlat>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_IDMAP;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryFlat>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_BIN_IDMAP;
        }
    }

 private:
    std::unique_ptr<IndexType> index_;
    std::shared_ptr<ThreadPool> search_pool_;
};

KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(FLAT, FlatIndexNode,
                                              knowhere::feature::NO_TRAIN | knowhere::feature::KNN |
                                                  knowhere::feature::MMAP,
                                              faiss::IndexFlat);

KNOWHERE_MOCK_REGISTER_DENSE_INT_GLOBAL(FLAT, FlatIndexNode,
                                        knowhere::feature::NO_TRAIN | knowhere::feature::KNN | knowhere::feature::MMAP,
                                        faiss::IndexFlat);

KNOWHERE_SIMPLE_REGISTER_DENSE_BIN_GLOBAL(BINFLAT, FlatIndexNode,
                                          knowhere::feature::NO_TRAIN | knowhere::feature::KNN |
                                              knowhere::feature::MMAP,
                                          faiss::IndexBinaryFlat);
KNOWHERE_SIMPLE_REGISTER_DENSE_BIN_GLOBAL(BIN_FLAT, FlatIndexNode,
                                          knowhere::feature::NO_TRAIN | knowhere::feature::KNN |
                                              knowhere::feature::MMAP,
                                          faiss::IndexBinaryFlat);
}  // namespace knowhere
