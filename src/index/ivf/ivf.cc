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
#include "faiss/IndexBinaryIVF.h"
#include "faiss/IndexFlat.h"
#include "faiss/IndexFlatElkan.h"
#include "faiss/IndexIVFFlat.h"
#include "faiss/IndexIVFPQ.h"
#include "faiss/IndexIVFPQFastScan.h"
#include "faiss/IndexIVFScalarQuantizerCC.h"
#include "faiss/IndexScaNN.h"
#include "faiss/IndexScalarQuantizer.h"
#include "faiss/impl/zerocopy_io.h"
#include "faiss/index_io.h"
#include "index/ivf/ivf_config.h"
#include "io/memory_io.h"
#include "knowhere/bitsetview_idselector.h"
#include "knowhere/comp/thread_pool.h"
#include "knowhere/dataset.h"
#include "knowhere/expected.h"
#include "knowhere/feature.h"
#include "knowhere/feder/IVFFlat.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/index/index_node_data_mock_wrapper.h"
#include "knowhere/log.h"
#include "knowhere/range_util.h"
#include "knowhere/utils.h"

namespace knowhere {
struct IVFBaseTag {};
struct IVFFlatTag {};

template <class IndexType>
struct IndexDispatch {
    using Tag = IVFBaseTag;
};

template <>
struct IndexDispatch<faiss::IndexIVFFlat> {
    using Tag = IVFFlatTag;
};

template <typename DataType, typename IndexType>
class IvfIndexNode : public IndexNode {
 public:
    IvfIndexNode(const int32_t version, const Object& object) : IndexNode(version), index_(nullptr) {
        static_assert(std::is_same<IndexType, faiss::IndexIVFFlat>::value ||
                          std::is_same<IndexType, faiss::IndexIVFFlatCC>::value ||
                          std::is_same<IndexType, faiss::IndexIVFPQ>::value ||
                          std::is_same<IndexType, faiss::IndexIVFScalarQuantizer>::value ||
                          std::is_same<IndexType, faiss::IndexBinaryIVF>::value ||
                          std::is_same<IndexType, faiss::IndexScaNN>::value ||
                          std::is_same<IndexType, faiss::IndexIVFScalarQuantizerCC>::value,
                      "not support");
        static_assert(std::is_same_v<DataType, fp32> || std::is_same_v<DataType, bin1>,
                      "IvfIndexNode only support float/binary");
        search_pool_ = ThreadPool::GetGlobalSearchThreadPool();
        build_pool_ = ThreadPool::GetGlobalBuildThreadPool();
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
    GetVectorByIds(const DataSetPtr dataset) const override;

    static Status
    StaticConfigCheck(const Config& cfg, PARAM_TYPE paramType, std::string& msg) {
        auto ivf_cfg = static_cast<const IvfConfig&>(cfg);

        if (paramType == PARAM_TYPE::TRAIN) {
            if constexpr (KnowhereFloatTypeCheck<DataType>::value) {
                if (IsMetricType(ivf_cfg.metric_type.value(), metric::L2) ||
                    IsMetricType(ivf_cfg.metric_type.value(), metric::IP) ||
                    IsMetricType(ivf_cfg.metric_type.value(), metric::COSINE)) {
                } else {
                    msg = "metric type " + ivf_cfg.metric_type.value() +
                          " not found or not supported, supported: [L2 IP COSINE]";
                    return Status::invalid_metric_type;
                }
            } else {
                if (IsMetricType(ivf_cfg.metric_type.value(), metric::HAMMING) ||
                    IsMetricType(ivf_cfg.metric_type.value(), metric::JACCARD)) {
                } else {
                    msg = "metric type " + ivf_cfg.metric_type.value() +
                          " not found or not supported, supported: [HAMMING JACCARD]";
                    return Status::invalid_metric_type;
                }
            }
        }
        return Status::success;
    }

    static bool
    CommonHasRawData() {
        if constexpr (std::is_same<faiss::IndexIVFFlat, IndexType>::value) {
            return true;
        }
        if constexpr (std::is_same<faiss::IndexIVFFlatCC, IndexType>::value) {
            return true;
        }
        if constexpr (std::is_same<faiss::IndexIVFPQ, IndexType>::value) {
            return false;
        }
        if constexpr (std::is_same<faiss::IndexIVFScalarQuantizer, IndexType>::value) {
            return false;
        }
        if constexpr (std::is_same<faiss::IndexBinaryIVF, IndexType>::value) {
            return true;
        }
        return false;
    }

    static bool
    StaticHasRawData(const knowhere::BaseConfig& config, const IndexVersion& version) {
        if constexpr (std::is_same<faiss::IndexScaNN, IndexType>::value) {
            const ScannConfig& scann_cfg = static_cast<const ScannConfig&>(config);
            return scann_cfg.with_raw_data.has_value() && scann_cfg.with_raw_data.value();
        }
        if constexpr (std::is_same<faiss::IndexIVFScalarQuantizerCC, IndexType>::value) {
            const IvfSqCcConfig& ivfsqcc_cfg = static_cast<const IvfSqCcConfig&>(config);
            return ivfsqcc_cfg.raw_data_store_prefix.has_value();
        }
        return CommonHasRawData();
    }

    bool
    HasRawData(const std::string& metric_type) const override {
        if (!index_) {
            return false;
        }
        if constexpr (std::is_same<faiss::IndexScaNN, IndexType>::value) {
            return index_->with_raw_data();
        }
        if constexpr (std::is_same<faiss::IndexIVFScalarQuantizerCC, IndexType>::value) {
            return index_->with_raw_data();
        }
        return CommonHasRawData();
    }
    expected<DataSetPtr>
    GetIndexMeta(std::unique_ptr<Config> cfg) const override {
        return this->GetIndexMetaImpl(std::move(cfg), typename IndexDispatch<IndexType>::Tag{});
    }
    Status
    Serialize(BinarySet& binset) const override {
        return this->SerializeImpl(binset, typename IndexDispatch<IndexType>::Tag{});
    }
    Status
    Deserialize(BinarySet&& binset, std::shared_ptr<Config> cfg) override;
    Status
    DeserializeFromFile(const std::string& filename, std::shared_ptr<Config> cfg) override;

    static std::unique_ptr<BaseConfig>
    StaticCreateConfig() {
        if constexpr (std::is_same<faiss::IndexIVFFlat, IndexType>::value) {
            return std::make_unique<IvfFlatConfig>();
        }
        if constexpr (std::is_same<faiss::IndexIVFFlatCC, IndexType>::value) {
            return std::make_unique<IvfFlatCcConfig>();
        }
        if constexpr (std::is_same<faiss::IndexIVFPQ, IndexType>::value) {
            return std::make_unique<IvfPqConfig>();
        }
        if constexpr (std::is_same<faiss::IndexScaNN, IndexType>::value) {
            return std::make_unique<ScannConfig>();
        }
        if constexpr (std::is_same<faiss::IndexIVFScalarQuantizer, IndexType>::value) {
            return std::make_unique<IvfSqConfig>();
        }
        if constexpr (std::is_same<faiss::IndexBinaryIVF, IndexType>::value) {
            return std::make_unique<IvfBinConfig>();
        }
        if constexpr (std::is_same<faiss::IndexIVFScalarQuantizerCC, IndexType>::value) {
            return std::make_unique<IvfSqCcConfig>();
        }
    };

    std::unique_ptr<BaseConfig>
    CreateConfig() const override {
        return StaticCreateConfig();
    };

    int64_t
    Dim() const override {
        if (!index_) {
            return -1;
        }
        return index_->d;
    };
    int64_t
    Size() const override {
        if (!index_) {
            return 0;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFFlat>::value) {
            auto nb = index_->invlists->compute_ntotal();
            auto nlist = index_->nlist;
            auto code_size = index_->code_size;
            return ((nb + nlist) * (code_size + sizeof(int64_t)));
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFFlatCC>::value) {
            auto nb = index_->invlists->compute_ntotal();
            auto nlist = index_->nlist;
            auto code_size = index_->code_size;
            return (nb * code_size + nb * sizeof(int64_t) + nlist * code_size);
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFPQ>::value) {
            auto nb = index_->invlists->compute_ntotal();
            auto code_size = index_->code_size;
            auto pq = index_->pq;
            auto nlist = index_->nlist;
            auto d = index_->d;

            auto capacity = nb * code_size + nb * sizeof(int64_t) + nlist * d * sizeof(float);
            auto centroid_table = pq.M * pq.ksub * pq.dsub * sizeof(float);
            auto precomputed_table = nlist * pq.M * pq.ksub * sizeof(float);
            return (capacity + centroid_table + precomputed_table);
        }
        if constexpr (std::is_same<IndexType, faiss::IndexScaNN>::value) {
            return index_->size();
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFScalarQuantizer>::value) {
            auto nb = index_->invlists->compute_ntotal();
            auto code_size = index_->code_size;
            auto nlist = index_->nlist;
            return (nb * code_size + nb * sizeof(int64_t) + 2 * code_size + nlist * code_size);
        }
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
            auto nb = index_->invlists->compute_ntotal();
            auto nlist = index_->nlist;
            auto code_size = index_->code_size;
            return (nb * code_size + nb * sizeof(int64_t) + nlist * code_size);
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFScalarQuantizerCC>::value) {
            auto nb = index_->invlists->compute_ntotal();
            auto code_size = index_->code_size;
            auto nlist = index_->nlist;
            return (nb * code_size + nb * sizeof(int64_t) + 2 * code_size + nlist * sizeof(float));
        }
    };
    int64_t
    Count() const override {
        if (!index_) {
            return 0;
        }
        return index_->ntotal;
    };
    std::string
    Type() const override {
        if constexpr (std::is_same<IndexType, faiss::IndexIVFFlat>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_IVFFLAT;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFFlatCC>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_IVFFLAT_CC;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFPQ>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_IVFPQ;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexScaNN>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_SCANN;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFScalarQuantizer>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_IVFSQ8;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_BIN_IVFFLAT;
        }
        if constexpr (std::is_same<IndexType, faiss::IndexIVFScalarQuantizerCC>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_IVFSQ_CC;
        }
    };

 private:
    expected<DataSetPtr>
    GetIndexMetaImpl(std::unique_ptr<Config> cfg, IVFBaseTag) const {
        return expected<DataSetPtr>::Err(Status::not_implemented, "GetIndexMeta not implemented");
    }
    expected<DataSetPtr>
    GetIndexMetaImpl(std::unique_ptr<Config> cfg, IVFFlatTag) const;

    Status
    SerializeImpl(BinarySet& binset, IVFBaseTag) const;

    Status
    SerializeImpl(BinarySet& binset, IVFFlatTag) const;

    Status
    TrainInternal(const DataSetPtr dataset, std::shared_ptr<Config> cfg);

    static constexpr bool
    IsQuantized() {
        return std::is_same_v<IndexType, faiss::IndexIVFPQ> ||
               std::is_same_v<IndexType, faiss::IndexIVFScalarQuantizer> ||
               std::is_same_v<IndexType, faiss::IndexIVFScalarQuantizerCC> ||
               std::is_same_v<IndexType, faiss::IndexScaNN>;
    }

 private:
    // only support IVFFlat,IVFFlatCC, IVFSQ, IVFSQCC and SCANN
    // iterator will own the copied_norm_query
    // TODO: iterator should copy and own query data.
    // TODO: If SCANN support Iterator, raw_distance() function should be override.
    class iterator : public IndexIterator {
     public:
        iterator(const IndexType* index, std::unique_ptr<float[]>&& copied_query, const BitsetView& bitset,
                 size_t nprobe, bool larger_is_closer, const float refine_ratio = 0.5f,
                 bool use_knowhere_search_pool = true)
            : IndexIterator(larger_is_closer, use_knowhere_search_pool, refine_ratio),
              index_(index),
              copied_query_(std::move(copied_query)) {
            if (!bitset.empty()) {
                bw_idselector_ = std::make_unique<BitsetViewIDSelector>(bitset);
                ivf_search_params_.sel = bw_idselector_.get();
            }

            ivf_search_params_.nprobe = nprobe;
            ivf_search_params_.max_codes = 0;

            workspace_ = index_->getIteratorWorkspace(copied_query_.get(), &ivf_search_params_);
        }

     protected:
        void
        next_batch(std::function<void(const std::vector<DistId>&)> batch_handler) override {
            index_->getIteratorNextBatch(workspace_.get(), this->res_.size());
            batch_handler(workspace_->dists);
            workspace_->dists.clear();
        }

        float
        raw_distance(int64_t id) override {
            if constexpr (std::is_same_v<IndexType, faiss::IndexScaNN>) {
                if (this->refine_) {
                    return workspace_->dis_refine->operator()(id);
                } else {
                    throw std::runtime_error("raw_distance should not be called if refine == false");
                }
            }
            throw std::runtime_error("raw_distance not implemented");
        }

     private:
        const IndexType* index_ = nullptr;
        std::unique_ptr<faiss::IVFIteratorWorkspace> workspace_ = nullptr;
        std::unique_ptr<float[]> copied_query_ = nullptr;
        std::unique_ptr<BitsetViewIDSelector> bw_idselector_ = nullptr;
        faiss::IVFSearchParameters ivf_search_params_;
    };

    std::unique_ptr<IndexType> index_;
    std::shared_ptr<ThreadPool> search_pool_;
    // Faiss uses OpenMP for training/building the index and we have no control
    // over those threads. build_pool_ is used to make sure the OMP threads
    // spawded during index training/building can inherit the low nice value of
    // threads in build_pool_.
    std::shared_ptr<ThreadPool> build_pool_;
};

}  // namespace knowhere

namespace knowhere {

inline int64_t
MatchNlist(int64_t size, int64_t nlist) {
    const int64_t MIN_POINTS_PER_CENTROID = 39;

    if (nlist * MIN_POINTS_PER_CENTROID > size) {
        // nlist is too large, adjust to a proper value
        LOG_KNOWHERE_WARNING_ << "nlist(" << nlist << ") is too large, adjust to a proper value";
        nlist = std::max(static_cast<int64_t>(1), size / MIN_POINTS_PER_CENTROID);
        LOG_KNOWHERE_WARNING_ << "Row num " << size << " match nlist " << nlist;
    }
    return nlist;
}

int64_t
MatchNbits(int64_t size, int64_t nbits) {
    if (size < (1 << nbits)) {
        // nbits is too large, adjust to a proper value
        LOG_KNOWHERE_WARNING_ << "nbits(" << nbits << ") is too large, adjust to a proper value";
        if (size >= (1 << 8)) {
            nbits = 8;
        } else if (size >= (1 << 4)) {
            nbits = 4;
        } else if (size >= (1 << 2)) {
            nbits = 2;
        } else {
            nbits = 1;
        }
        LOG_KNOWHERE_WARNING_ << "Row num " << size << " match nbits " << nbits;
    }
    return nbits;
}

namespace {

// turn IndexFlatElkan into IndexFlat
std::unique_ptr<faiss::IndexFlat>
to_index_flat(std::unique_ptr<faiss::IndexFlat>&& index) {
    // C++ slicing here
    return std::make_unique<faiss::IndexFlat>(std::move(*index));
}

expected<faiss::ScalarQuantizer::QuantizerType>
get_ivf_sq_quantizer_type(int code_size) {
    switch (code_size) {
        case 4:
            return faiss::ScalarQuantizer::QuantizerType::QT_4bit;
        case 6:
            return faiss::ScalarQuantizer::QuantizerType::QT_6bit;
        case 8:
            return faiss::ScalarQuantizer::QuantizerType::QT_8bit;
        case 16:
            return faiss::ScalarQuantizer::QuantizerType::QT_fp16;
        default:
            return expected<faiss::ScalarQuantizer::QuantizerType>::Err(
                Status::invalid_args, fmt::format("current code size {} not in (4, 6, 8, 16)", code_size));
    }
}
}  // namespace

template <typename DataType, typename IndexType>
Status
IvfIndexNode<DataType, IndexType>::Train(const DataSetPtr dataset, std::shared_ptr<Config> cfg) {
    // use build_pool_ to make sure the OMP threads spawded by index_->train etc
    // can inherit the low nice value of threads in build_pool_.
    auto tryObj = build_pool_->push([&] { return TrainInternal(dataset, std::move(cfg)); }).getTry();
    if (tryObj.hasValue()) {
        return tryObj.value();
    }
    LOG_KNOWHERE_WARNING_ << "faiss internal error: " << tryObj.exception().what();
    return Status::faiss_inner_error;
}

template <typename DataType, typename IndexType>
Status
IvfIndexNode<DataType, IndexType>::TrainInternal(const DataSetPtr dataset, std::shared_ptr<Config> cfg) {
    const BaseConfig& base_cfg = static_cast<const IvfConfig&>(*cfg);
    std::unique_ptr<ThreadPool::ScopedBuildOmpSetter> setter;
    if (base_cfg.num_build_thread.has_value()) {
        setter = std::make_unique<ThreadPool::ScopedBuildOmpSetter>(base_cfg.num_build_thread.value());
    } else {
        setter = std::make_unique<ThreadPool::ScopedBuildOmpSetter>();
    }

    bool is_cosine = IsMetricType(base_cfg.metric_type.value(), knowhere::metric::COSINE);

    // do normalize for COSINE metric type
    if constexpr (std::is_same_v<faiss::IndexIVFPQ, IndexType> ||
                  std::is_same_v<faiss::IndexIVFScalarQuantizer, IndexType>) {
        if (is_cosine) {
            NormalizeDataset<DataType>(dataset);
        }
    }

    auto metric = Str2FaissMetricType(base_cfg.metric_type.value());
    if (!metric.has_value()) {
        LOG_KNOWHERE_ERROR_ << "Invalid metric type: " << base_cfg.metric_type.value();
        return Status::invalid_metric_type;
    }

    auto rows = dataset->GetRows();
    auto dim = dataset->GetDim();
    auto data = dataset->GetTensor();

    // faiss scann needs at least 16 rows since nbits=4
    constexpr int64_t SCANN_MIN_ROWS = 16;
    if constexpr (std::is_same<faiss::IndexScaNN, IndexType>::value) {
        if (rows < SCANN_MIN_ROWS) {
            LOG_KNOWHERE_ERROR_ << rows << " rows is not enough, scann needs at least 16 rows to build index";
            return Status::faiss_inner_error;
        }
    }

    std::unique_ptr<IndexType> index;
    // if cfg.use_elkan is used, then we'll use a temporary instance of
    //  IndexFlatElkan for the training.
    if constexpr (std::is_same<faiss::IndexIVFFlat, IndexType>::value) {
        const IvfFlatConfig& ivf_flat_cfg = static_cast<const IvfFlatConfig&>(*cfg);
        auto nlist = MatchNlist(rows, ivf_flat_cfg.nlist.value());

        const bool use_elkan = ivf_flat_cfg.use_elkan.value_or(true);

        // create quantizer for the training
        std::unique_ptr<faiss::IndexFlat> qzr =
            std::make_unique<faiss::IndexFlatElkan>(dim, metric.value(), false, use_elkan);
        // create index. Index does not own qzr
        index = std::make_unique<faiss::IndexIVFFlat>(qzr.get(), dim, nlist, metric.value(), is_cosine);
        // train
        index->train(rows, (const float*)data);
        // replace quantizer with a regular IndexFlat
        qzr = to_index_flat(std::move(qzr));
        // transfer ownership of qzr to index
        index->quantizer = qzr.release();
        index->own_fields = true;
    }
    if constexpr (std::is_same<faiss::IndexIVFFlatCC, IndexType>::value) {
        const IvfFlatCcConfig& ivf_flat_cc_cfg = static_cast<const IvfFlatCcConfig&>(*cfg);
        auto nlist = MatchNlist(rows, ivf_flat_cc_cfg.nlist.value());

        const bool use_elkan = ivf_flat_cc_cfg.use_elkan.value_or(true);

        // create quantizer for the training
        std::unique_ptr<faiss::IndexFlat> qzr =
            std::make_unique<faiss::IndexFlatElkan>(dim, metric.value(), false, use_elkan);
        // create index. Index does not own qzr
        index = std::make_unique<faiss::IndexIVFFlatCC>(qzr.get(), dim, nlist, ivf_flat_cc_cfg.ssize.value(),
                                                        metric.value(), is_cosine);
        // train
        index->train(rows, (const float*)data);
        // replace quantizer with a regular IndexFlat
        qzr = to_index_flat(std::move(qzr));
        // transfer ownership of qzr to index
        index->quantizer = qzr.release();
        index->own_fields = true;
        // ivfflat_cc has no serialize stage, make map at build stage
        index->make_direct_map(true, faiss::DirectMap::ConcurrentArray);
    }
    if constexpr (std::is_same<faiss::IndexIVFPQ, IndexType>::value) {
        const IvfPqConfig& ivf_pq_cfg = static_cast<const IvfPqConfig&>(*cfg);
        auto nlist = MatchNlist(rows, ivf_pq_cfg.nlist.value());
        auto nbits = MatchNbits(rows, ivf_pq_cfg.nbits.value());

        const bool use_elkan = ivf_pq_cfg.use_elkan.value_or(true);

        // create quantizer for the training
        std::unique_ptr<faiss::IndexFlat> qzr =
            std::make_unique<faiss::IndexFlatElkan>(dim, metric.value(), false, use_elkan);
        // create index. Index does not own qzr
        index = std::make_unique<faiss::IndexIVFPQ>(qzr.get(), dim, nlist, ivf_pq_cfg.m.value(), nbits, metric.value());
        // train
        index->train(rows, (const float*)data);
        // replace quantizer with a regular IndexFlat
        qzr = to_index_flat(std::move(qzr));
        // transfer ownership of qzr to index
        index->quantizer = qzr.release();
        index->own_fields = true;
    }
    if constexpr (std::is_same<faiss::IndexScaNN, IndexType>::value) {
        const ScannConfig& scann_cfg = static_cast<const ScannConfig&>(*cfg);
        auto nlist = MatchNlist(rows, scann_cfg.nlist.value());
        bool is_cosine = base_cfg.metric_type.value() == metric::COSINE;

        const bool use_elkan = scann_cfg.use_elkan.value_or(true);

        // create quantizer for the training
        std::unique_ptr<faiss::IndexFlat> qzr =
            std::make_unique<faiss::IndexFlatElkan>(dim, metric.value(), false, use_elkan);
        // create base index. it does not own qzr
        auto base_index = std::make_unique<faiss::IndexIVFPQFastScan>(qzr.get(), dim, nlist, (dim + 1) / 2, 4,
                                                                      is_cosine, metric.value());
        // create scann index, which does not base_index by default,
        //    but owns the refine index by default omg
        if (scann_cfg.with_raw_data.value()) {
            index = std::make_unique<faiss::IndexScaNN>(base_index.get(), (const float*)data);
        } else {
            index = std::make_unique<faiss::IndexScaNN>(base_index.get(), nullptr);
        }
        // train
        index->train(rows, (const float*)data);
        // at this moment, we still own qzr.
        // replace quantizer with a regular IndexFlat
        qzr = to_index_flat(std::move(qzr));
        // release qzr
        base_index->quantizer = qzr.release();
        base_index->own_fields = true;
        // transfer ownership of the base index
        base_index.release();
        index->own_fields = true;
    }
    if constexpr (std::is_same<faiss::IndexIVFScalarQuantizer, IndexType>::value) {
        const IvfSqConfig& ivf_sq_cfg = static_cast<const IvfSqConfig&>(*cfg);
        auto nlist = MatchNlist(rows, ivf_sq_cfg.nlist.value());

        const bool use_elkan = ivf_sq_cfg.use_elkan.value_or(true);

        // create quantizer for the training
        std::unique_ptr<faiss::IndexFlat> qzr =
            std::make_unique<faiss::IndexFlatElkan>(dim, metric.value(), false, use_elkan);
        // create index. Index does not own qzr
        index = std::make_unique<faiss::IndexIVFScalarQuantizer>(
            qzr.get(), dim, nlist, faiss::ScalarQuantizer::QuantizerType::QT_8bit, metric.value());
        // train
        index->train(rows, (const float*)data);
        // replace quantizer with a regular IndexFlat
        qzr = to_index_flat(std::move(qzr));
        // transfer ownership of qzr to index
        index->quantizer = qzr.release();
        index->own_fields = true;
    }
    if constexpr (std::is_same<faiss::IndexBinaryIVF, IndexType>::value) {
        const IvfBinConfig& ivf_bin_cfg = static_cast<const IvfBinConfig&>(*cfg);
        auto nlist = MatchNlist(rows, ivf_bin_cfg.nlist.value());

        // create quantizer
        auto qzr = std::make_unique<faiss::IndexBinaryFlat>(dim, metric.value());
        // create index. Index does not own qzr
        index = std::make_unique<faiss::IndexBinaryIVF>(qzr.get(), dim, nlist, metric.value());
        // train
        index->train(rows, (const uint8_t*)data);
        // transfer ownership of qzr to index
        qzr.release();
        index->own_fields = true;
    }
    if constexpr (std::is_same<faiss::IndexIVFScalarQuantizerCC, IndexType>::value) {
        const IvfSqCcConfig& ivf_sq_cc_cfg = static_cast<const IvfSqCcConfig&>(*cfg);
        auto nlist = MatchNlist(rows, ivf_sq_cc_cfg.nlist.value());
        auto ssize = ivf_sq_cc_cfg.ssize.value();

        const bool use_elkan = ivf_sq_cc_cfg.use_elkan.value_or(true);

        // create quantizer for the training
        std::unique_ptr<faiss::IndexFlat> qzr =
            std::make_unique<faiss::IndexFlatElkan>(dim, metric.value(), false, use_elkan);
        // create index. Index does not own qzr
        auto qzr_type = get_ivf_sq_quantizer_type(ivf_sq_cc_cfg.code_size.value());
        if (!qzr_type.has_value()) {
            LOG_KNOWHERE_ERROR_ << "fail to get ivf sq quantizer type, " << qzr_type.what();
            return qzr_type.error();
        }
        index = std::make_unique<faiss::IndexIVFScalarQuantizerCC>(qzr.get(), dim, nlist, ssize, qzr_type.value(),
                                                                   metric.value(), is_cosine, false,
                                                                   ivf_sq_cc_cfg.raw_data_store_prefix);
        // train
        index->train(rows, (const float*)data);
        // replace quantizer with a regular IndexFlat
        qzr = to_index_flat(std::move(qzr));
        // transfer ownership of qzr to index
        index->quantizer = qzr.release();
        index->own_fields = true;
        index->make_direct_map(true, faiss::DirectMap::ConcurrentArray);
    }
    index_ = std::move(index);

    return Status::success;
}

template <typename DataType, typename IndexType>
Status
IvfIndexNode<DataType, IndexType>::Add(const DataSetPtr dataset, std::shared_ptr<Config> cfg) {
    if (!this->index_) {
        LOG_KNOWHERE_ERROR_ << "Can not add data to empty IVF index.";
        return Status::empty_index;
    }
    auto data = dataset->GetTensor();
    auto rows = dataset->GetRows();
    const BaseConfig& base_cfg = static_cast<const IvfConfig&>(*cfg);
    // use build_pool_ to make sure the OMP threads spawded by index_->add
    // can inherit the low nice value of threads in build_pool_.
    auto tryObj = build_pool_
                      ->push([&] {
                          std::unique_ptr<ThreadPool::ScopedBuildOmpSetter> setter;
                          if (base_cfg.num_build_thread.has_value()) {
                              setter =
                                  std::make_unique<ThreadPool::ScopedBuildOmpSetter>(base_cfg.num_build_thread.value());
                          } else {
                              setter = std::make_unique<ThreadPool::ScopedBuildOmpSetter>();
                          }
                          if constexpr (std::is_same<faiss::IndexBinaryIVF, IndexType>::value) {
                              index_->add(rows, (const uint8_t*)data);
                          } else {
                              index_->add(rows, (const float*)data);
                          }
                      })
                      .getTry();
    if (tryObj.hasException()) {
        LOG_KNOWHERE_WARNING_ << "faiss internal error: " << tryObj.exception().what();
        return Status::faiss_inner_error;
    }
    return Status::success;
}

template <typename DataType, typename IndexType>
expected<DataSetPtr>
IvfIndexNode<DataType, IndexType>::Search(const DataSetPtr dataset, std::unique_ptr<Config> cfg,
                                          const BitsetView& bitset) const {
    if (!this->index_) {
        LOG_KNOWHERE_WARNING_ << "search on empty index";
        return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
    }
    if (!this->index_->is_trained) {
        LOG_KNOWHERE_WARNING_ << "index not trained";
        return expected<DataSetPtr>::Err(Status::index_not_trained, "index not trained");
    }

    auto dim = dataset->GetDim();
    auto rows = dataset->GetRows();
    auto data = dataset->GetTensor();

    const IvfConfig& ivf_cfg = static_cast<const IvfConfig&>(*cfg);
    bool is_cosine = IsMetricType(ivf_cfg.metric_type.value(), knowhere::metric::COSINE);

    auto k = ivf_cfg.k.value();
    auto nprobe = ivf_cfg.nprobe.value();

    auto ids = std::make_unique<int64_t[]>(rows * k);
    auto distances = std::make_unique<float[]>(rows * k);
    try {
        std::vector<folly::Future<folly::Unit>> futs;
        futs.reserve(rows);
        for (int i = 0; i < rows; ++i) {
            futs.emplace_back(search_pool_->push([&, index = i] {
                ThreadPool::ScopedSearchOmpSetter setter(1);
                auto offset = k * index;
                std::unique_ptr<float[]> copied_query = nullptr;

                BitsetViewIDSelector bw_idselector(bitset);
                faiss::IDSelector* id_selector = (bitset.empty()) ? nullptr : &bw_idselector;

                if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
                    auto cur_data = (const uint8_t*)data + index * ((dim + 7) / 8);

                    int32_t* i_distances = reinterpret_cast<int32_t*>(distances.get());

                    faiss::IVFSearchParameters ivf_search_params;
                    ivf_search_params.nprobe = nprobe;
                    ivf_search_params.sel = id_selector;
                    index_->search(1, cur_data, k, i_distances + offset, ids.get() + offset, &ivf_search_params);

                    if (index_->metric_type == faiss::METRIC_Hamming) {
                        // this is an in-place conversion int32_t -> float
                        for (int64_t i = 0; i < k; i++) {
                            distances[i + offset] = static_cast<float>(i_distances[i + offset]);
                        }
                    }
                } else if constexpr (std::is_same<IndexType, faiss::IndexIVFFlatCC>::value ||
                                     std::is_same<IndexType, faiss::IndexIVFScalarQuantizerCC>::value) {
                    auto cur_query = (const float*)data + index * dim;
                    if (is_cosine) {
                        copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                        cur_query = copied_query.get();
                    }

                    faiss::IVFSearchParameters ivf_search_params;

                    ivf_search_params.sel = id_selector;
                    ivf_search_params.ensure_topk_full = ivf_cfg.ensure_topk_full.value();
                    if (ivf_search_params.ensure_topk_full) {
                        ivf_search_params.nprobe = index_->nlist;
                        // use max_codes to early termination
                        ivf_search_params.max_codes =
                            (nprobe * 1.0 / index_->nlist) * (index_->ntotal - bitset.count());
                    } else {
                        ivf_search_params.nprobe = nprobe;
                        ivf_search_params.max_codes = 0;
                    }

                    index_->search(1, cur_query, k, distances.get() + offset, ids.get() + offset, &ivf_search_params);
                } else if constexpr (std::is_same<IndexType, faiss::IndexScaNN>::value) {
                    auto cur_query = (const float*)data + index * dim;
                    const ScannConfig& scann_cfg = static_cast<const ScannConfig&>(*cfg);
                    if (is_cosine) {
                        copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                        cur_query = copied_query.get();
                    }

                    // todo aguzhva: this is somewhat alogical. Refactor?
                    faiss::IVFSearchParameters base_search_params;
                    base_search_params.sel = id_selector;
                    base_search_params.nprobe = nprobe;

                    faiss::IndexScaNNSearchParameters scann_search_params;
                    scann_search_params.base_index_params = &base_search_params;
                    scann_search_params.reorder_k = scann_cfg.reorder_k.value();

                    index_->search(1, cur_query, k, distances.get() + offset, ids.get() + offset, &scann_search_params);
                } else {
                    auto cur_query = (const float*)data + index * dim;
                    if (is_cosine) {
                        copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                        cur_query = copied_query.get();
                    }

                    faiss::IVFSearchParameters ivf_search_params;
                    ivf_search_params.nprobe = nprobe;
                    ivf_search_params.max_codes = 0;
                    ivf_search_params.sel = id_selector;

                    index_->search(1, cur_query, k, distances.get() + offset, ids.get() + offset, &ivf_search_params);
                }
            }));
        }
        // wait for the completion
        WaitAllSuccess(futs);
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
        return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
    }

    auto res = GenResultDataSet(rows, k, std::move(ids), std::move(distances));
    return res;
}

template <typename DataType, typename IndexType>
expected<DataSetPtr>
IvfIndexNode<DataType, IndexType>::RangeSearch(const DataSetPtr dataset, std::unique_ptr<Config> cfg,
                                               const BitsetView& bitset) const {
    if (!this->index_) {
        LOG_KNOWHERE_WARNING_ << "range search on empty index";
        return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
    }
    if (!this->index_->is_trained) {
        LOG_KNOWHERE_WARNING_ << "index not trained";
        return expected<DataSetPtr>::Err(Status::index_not_trained, "index not trained");
    }

    auto nq = dataset->GetRows();
    auto xq = dataset->GetTensor();
    auto dim = dataset->GetDim();

    const IvfConfig& ivf_cfg = static_cast<const IvfConfig&>(*cfg);
    bool is_cosine = IsMetricType(ivf_cfg.metric_type.value(), knowhere::metric::COSINE);

    float radius = ivf_cfg.radius.value();
    float range_filter = ivf_cfg.range_filter.value();
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
                std::unique_ptr<float[]> copied_query = nullptr;

                BitsetViewIDSelector bw_idselector(bitset);
                faiss::IDSelector* id_selector = (bitset.empty()) ? nullptr : &bw_idselector;

                if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
                    auto cur_data = (const uint8_t*)xq + index * ((dim + 7) / 8);

                    faiss::IVFSearchParameters ivf_search_params;
                    ivf_search_params.nprobe = index_->nlist;
                    ivf_search_params.max_empty_result_buckets = ivf_cfg.max_empty_result_buckets.value();
                    ivf_search_params.sel = id_selector;

                    index_->range_search(1, cur_data, radius, &res, &ivf_search_params);
                } else if constexpr (std::is_same<IndexType, faiss::IndexIVFFlat>::value) {
                    auto cur_query = (const float*)xq + index * dim;
                    if (is_cosine) {
                        copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                        cur_query = copied_query.get();
                    }

                    faiss::IVFSearchParameters ivf_search_params;
                    ivf_search_params.nprobe = index_->nlist;
                    ivf_search_params.max_codes = 0;
                    ivf_search_params.max_empty_result_buckets = ivf_cfg.max_empty_result_buckets.value();
                    ivf_search_params.sel = id_selector;

                    index_->range_search(1, cur_query, radius, &res, &ivf_search_params);
                } else if constexpr (std::is_same<IndexType, faiss::IndexScaNN>::value) {
                    auto cur_query = (const float*)xq + index * dim;
                    if (is_cosine) {
                        copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                        cur_query = copied_query.get();
                    }

                    // todo aguzhva: this is somewhat alogical. Refactor?
                    faiss::IVFSearchParameters search_params;
                    search_params.max_empty_result_buckets = ivf_cfg.max_empty_result_buckets.value();
                    search_params.sel = id_selector;

                    index_->range_search(1, cur_query, radius, &res, &search_params);
                } else {
                    auto cur_query = (const float*)xq + index * dim;
                    if (is_cosine) {
                        copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                        cur_query = copied_query.get();
                    }

                    faiss::IVFSearchParameters ivf_search_params;
                    ivf_search_params.nprobe = index_->nlist;
                    ivf_search_params.max_codes = 0;
                    ivf_search_params.max_empty_result_buckets = ivf_cfg.max_empty_result_buckets.value();
                    ivf_search_params.sel = id_selector;

                    index_->range_search(1, cur_query, radius, &res, &ivf_search_params);
                }
                auto elem_cnt = res.lims[1];
                result_dist_array[index].resize(elem_cnt);
                result_id_array[index].resize(elem_cnt);
                for (size_t j = 0; j < elem_cnt; j++) {
                    result_dist_array[index][j] = res.distances[j];
                    result_id_array[index][j] = res.labels[j];
                }
                if (range_filter != defaultRangeFilter) {
                    FilterRangeSearchResultForOneNq(result_dist_array[index], result_id_array[index], is_ip, radius,
                                                    range_filter);
                }
            }));
        }
        // wait for the completion
        WaitAllSuccess(futs);
        range_search_result = GetRangeSearchResult(result_dist_array, result_id_array, is_ip, nq, radius, range_filter);
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
        return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
    }

    return GenResultDataSet(nq, std::move(range_search_result));
}

template <typename DataType, typename IndexType>
expected<std::vector<IndexNode::IteratorPtr>>
IvfIndexNode<DataType, IndexType>::AnnIterator(const DataSetPtr dataset, std::unique_ptr<Config> cfg,
                                               const BitsetView& bitset, bool use_knowhere_search_pool) const {
    if (!index_) {
        LOG_KNOWHERE_WARNING_ << "creating iterator on empty index";
        return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::empty_index, "index not loaded");
    }
    if (!index_->is_trained) {
        LOG_KNOWHERE_WARNING_ << "index not trained";
        return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::index_not_trained, "index not trained");
    }
    // only support IVFFlat, IVFFlatCC, IVF_SQ8 and IVF_SQ_CC;
    if constexpr (!std::is_same<faiss::IndexIVFFlatCC, IndexType>::value &&
                  !std::is_same<faiss::IndexIVFFlat, IndexType>::value &&
                  !std::is_same<faiss::IndexIVFScalarQuantizer, IndexType>::value &&
                  !std::is_same<faiss::IndexIVFScalarQuantizerCC, IndexType>::value &&
                  !std::is_same<faiss::IndexScaNN, IndexType>::value) {
        LOG_KNOWHERE_WARNING_ << "Current index_type: " << Type()
                              << ", only IVFFlat, IVFFlatCC, IVF_SQ8, IVF_SQ_CC and SCANN support Iterator.";
        return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::not_implemented, "index not supported");
    } else {
        auto dim = dataset->GetDim();
        auto rows = dataset->GetRows();
        auto data = dataset->GetTensor();

        auto vec = std::vector<IndexNode::IteratorPtr>(rows, nullptr);

        const IvfConfig& ivf_cfg = static_cast<const IvfConfig&>(*cfg);
        bool is_cosine = IsMetricType(ivf_cfg.metric_type.value(), knowhere::metric::COSINE);
        auto larger_is_closer = IsMetricType(ivf_cfg.metric_type.value(), knowhere::metric::IP) || is_cosine;

        size_t nprobe = ivf_cfg.nprobe.value();
        // set iterator_refine_ratio = 0.0. If quantizer != flat, faiss:indexivf will not keep raw data;
        // TODO: if SCANN support Iterator, iterator_refine_ratio should be set.
        float iterator_refine_ratio = 0.0f;
        if constexpr (std::is_same_v<IndexType, faiss::IndexScaNN>) {
            if (HasRawData(ivf_cfg.metric_type.value())) {
                iterator_refine_ratio = ivf_cfg.iterator_refine_ratio.value();
            }
        }
        try {
            for (int i = 0; i < rows; ++i) {
                auto cur_query = (const float*)data + i * dim;
                // if cosine, need normalize
                std::unique_ptr<float[]> copied_query = nullptr;
                if (is_cosine) {
                    copied_query = CopyAndNormalizeVecs(cur_query, 1, dim);
                } else {
                    copied_query = std::make_unique<float[]>(dim);
                    std::copy_n(cur_query, dim, copied_query.get());
                }

                // iterator only own the copied_query.
                auto it = std::make_shared<iterator>(index_.get(), std::move(copied_query), bitset, nprobe,
                                                     larger_is_closer, iterator_refine_ratio, use_knowhere_search_pool);
                vec[i] = it;
            }

        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
            return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::faiss_inner_error, e.what());
        }
        return vec;
    }
}

template <typename DataType, typename IndexType>
expected<DataSetPtr>
IvfIndexNode<DataType, IndexType>::GetVectorByIds(const DataSetPtr dataset) const {
    if (!this->index_) {
        return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
    }
    if (!this->index_->is_trained) {
        return expected<DataSetPtr>::Err(Status::index_not_trained, "index not trained");
    }
    if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
        auto dim = Dim();
        auto rows = dataset->GetRows();
        auto ids = dataset->GetIds();

        try {
            auto data = std::make_unique<uint8_t[]>(rows * ((dim + 7) / 8));
            for (int64_t i = 0; i < rows; i++) {
                int64_t id = ids[i];
                assert(id >= 0 && id < index_->ntotal);
                index_->reconstruct(id, data.get() + i * ((dim + 7) / 8));
            }
            return GenResultDataSet(rows, dim, std::move(data));
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }
    } else if constexpr (std::is_same<IndexType, faiss::IndexIVFFlat>::value ||
                         std::is_same<IndexType, faiss::IndexIVFFlatCC>::value) {
        auto dim = Dim();
        auto rows = dataset->GetRows();
        auto ids = dataset->GetIds();

        try {
            auto data = std::make_unique<float[]>(dim * rows);
            for (int64_t i = 0; i < rows; i++) {
                int64_t id = ids[i];
                assert(id >= 0 && id < index_->ntotal);
                index_->reconstruct(id, data.get() + i * dim);
            }
            return GenResultDataSet(rows, dim, std::move(data));
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }
    } else if constexpr (std::is_same<IndexType, faiss::IndexScaNN>::value ||
                         std::is_same<IndexType, faiss::IndexIVFScalarQuantizerCC>::value) {
        // we should never go here since we should call HasRawData() first
        if (!index_->with_raw_data()) {
            return expected<DataSetPtr>::Err(Status::not_implemented, "GetVectorByIds not implemented");
        }
        auto dim = Dim();
        auto rows = dataset->GetRows();
        auto ids = dataset->GetIds();

        try {
            auto data = std::make_unique<float[]>(dim * rows);
            for (int64_t i = 0; i < rows; i++) {
                int64_t id = ids[i];
                assert(id >= 0 && id < index_->ntotal);
                index_->reconstruct(id, data.get() + i * dim);
            }
            return GenResultDataSet(rows, dim, std::move(data));
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }
    } else {
        return expected<DataSetPtr>::Err(Status::not_implemented, "GetVectorByIds not implemented");
    }
}

template <typename DataType, typename IndexType>
expected<DataSetPtr>
IvfIndexNode<DataType, IndexType>::GetIndexMetaImpl(std::unique_ptr<Config>, IVFFlatTag) const {
    if (!index_) {
        LOG_KNOWHERE_WARNING_ << "get index meta on empty index";
        return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
    }

    auto ivf_index = dynamic_cast<faiss::IndexIVF*>(index_.get());
    auto ivf_quantizer = dynamic_cast<faiss::IndexFlat*>(ivf_index->quantizer);

    int64_t dim = ivf_index->d;
    int64_t nlist = ivf_index->nlist;
    int64_t ntotal = ivf_index->ntotal;

    feder::ivfflat::IVFFlatMeta meta(nlist, dim, ntotal);
    std::unordered_set<int64_t> id_set;

    for (int32_t i = 0; i < nlist; i++) {
        // copy from IndexIVF::search_preassigned
        std::unique_ptr<faiss::InvertedLists::ScopedIds> sids =
            std::make_unique<faiss::InvertedLists::ScopedIds>(index_->invlists, i);

        // node ids
        auto node_num = index_->invlists->list_size(i);
        auto node_id_codes = sids->get();

        // centroid vector
        auto centroid_vec = ivf_quantizer->get_xb() + i * dim;

        meta.AddCluster(i, node_id_codes, node_num, centroid_vec, dim);
    }

    Json json_meta, json_id_set;
    nlohmann::to_json(json_meta, meta);
    nlohmann::to_json(json_id_set, id_set);
    return GenResultDataSet(json_meta.dump(), json_id_set.dump());
}

template <typename DataType, typename IndexType>
Status
IvfIndexNode<DataType, IndexType>::SerializeImpl(BinarySet& binset, IVFBaseTag) const {
    try {
        if (!this->index_) {
            LOG_KNOWHERE_WARNING_ << "index can not be serialized for empty index";
            return Status::empty_index;
        }
        MemoryIOWriter writer;
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
            faiss::write_index_binary(index_.get(), &writer);
        } else {
            faiss::write_index(index_.get(), &writer);
        }
        std::shared_ptr<uint8_t[]> data(writer.data());
        binset.Append(Type(), data, writer.tellg());
        return Status::success;
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
        return Status::faiss_inner_error;
    }
}

template <typename DataType, typename IndexType>
Status
IvfIndexNode<DataType, IndexType>::SerializeImpl(BinarySet& binset, IVFFlatTag) const {
    try {
        if (!this->index_) {
            LOG_KNOWHERE_WARNING_ << "index can not be serialized for empty index";
            return Status::empty_index;
        }
        MemoryIOWriter writer;
        LOG_KNOWHERE_INFO_ << "request version " << this->version_.VersionNumber();
        if (this->version_ <= Version::GetMinimalVersion()) {
            faiss::write_index_nm(index_.get(), &writer);
            LOG_KNOWHERE_INFO_ << "write IVF_FLAT_NM, file size " << writer.tellg();
        } else {
            faiss::write_index(index_.get(), &writer);
            LOG_KNOWHERE_INFO_ << "write IVF_FLAT, file size " << writer.tellg();
        }
        std::shared_ptr<uint8_t[]> index_data_ptr(writer.data());
        binset.Append(Type(), index_data_ptr, writer.tellg());

        // append raw data for backward compatible
        if (this->version_ <= Version::GetMinimalVersion()) {
            size_t dim = index_->d;
            size_t rows = index_->ntotal;
            size_t raw_data_size = dim * rows * sizeof(float);
            auto raw_data = std::make_unique<uint8_t[]>(raw_data_size);
            for (size_t i = 0; i < index_->nlist; i++) {
                size_t list_size = index_->invlists->list_size(i);
                const faiss::idx_t* ids = index_->invlists->get_ids(i);
                const uint8_t* codes = index_->invlists->get_codes(i);
                for (size_t j = 0; j < list_size; j++) {
                    faiss::idx_t id = ids[j];
                    const uint8_t* src = codes + j * dim * sizeof(float);
                    uint8_t* dst = raw_data.get() + id * dim * sizeof(float);
                    memcpy(dst, src, dim * sizeof(float));
                }
            }
            binset.Append("RAW_DATA", std::move(raw_data), raw_data_size);
            LOG_KNOWHERE_INFO_ << "append raw data for IVF_FLAT_NM, size " << raw_data_size;
        }
        return Status::success;
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
        return Status::faiss_inner_error;
    }
}

template <typename DataType, typename IndexType>
Status
IvfIndexNode<DataType, IndexType>::Deserialize(BinarySet&& binset, std::shared_ptr<Config> cfg) {
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
    try {
        if constexpr (std::is_same<IndexType, faiss::IndexIVFFlat>::value) {
            if (this->version_ <= Version::GetMinimalVersion()) {
                auto raw_binary = binset.GetByName("RAW_DATA");
                const BaseConfig& base_cfg = static_cast<const BaseConfig&>(*cfg);
                ConvertIVFFlat(binset, base_cfg.metric_type.value(), raw_binary->data.get(), raw_binary->size);
                // after conversion, binary size and data will be updated
                reader.data_ = binary->data.get();
                reader.total_ = binary->size;
            }
            index_.reset(static_cast<faiss::IndexIVFFlat*>(faiss::read_index(&reader, io_flags)));
        } else if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
            index_.reset(static_cast<IndexType*>(faiss::read_index_binary(&reader, io_flags)));
        } else {
            index_.reset(static_cast<IndexType*>(faiss::read_index(&reader, io_flags)));
        }
        if constexpr (!std::is_same_v<IndexType, faiss::IndexScaNN> &&
                      !std::is_same_v<IndexType, faiss::IndexIVFScalarQuantizerCC>) {
            const BaseConfig& base_cfg = static_cast<const BaseConfig&>(*cfg);
            if (HasRawData(base_cfg.metric_type.value())) {
                index_->make_direct_map(true);
            }
        }
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
        return Status::faiss_inner_error;
    }
    return Status::success;
}

template <typename DataType, typename IndexType>
Status
IvfIndexNode<DataType, IndexType>::DeserializeFromFile(const std::string& filename, std::shared_ptr<Config> config) {
    auto cfg = static_cast<const knowhere::BaseConfig&>(*config);

    int io_flags = 0;
    if (cfg.enable_mmap.value()) {
        io_flags |= faiss::IO_FLAG_MMAP;
    }
    try {
        if constexpr (std::is_same<IndexType, faiss::IndexBinaryIVF>::value) {
            index_.reset(static_cast<IndexType*>(faiss::read_index_binary(filename.data(), io_flags)));
        } else {
            index_.reset(static_cast<IndexType*>(faiss::read_index(filename.data(), io_flags)));
        }
        if constexpr (!std::is_same_v<IndexType, faiss::IndexScaNN>) {
            const BaseConfig& base_cfg = static_cast<const BaseConfig&>(*config);
            if (HasRawData(base_cfg.metric_type.value())) {
                index_->make_direct_map(true);
            }
        }
    } catch (const std::exception& e) {
        LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
        return Status::faiss_inner_error;
    }
    return Status::success;
}
// bin1
KNOWHERE_SIMPLE_REGISTER_DENSE_BIN_GLOBAL(IVFBIN, IvfIndexNode, knowhere::feature::MMAP, faiss::IndexBinaryIVF)
KNOWHERE_SIMPLE_REGISTER_DENSE_BIN_GLOBAL(BIN_IVF_FLAT, IvfIndexNode, knowhere::feature::MMAP, faiss::IndexBinaryIVF)

// float
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVFFLAT, IvfIndexNode, knowhere::feature::MMAP, faiss::IndexIVFFlat)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVF_FLAT, IvfIndexNode, knowhere::feature::MMAP, faiss::IndexIVFFlat)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVFFLATCC, IvfIndexNode, knowhere::feature::NONE, faiss::IndexIVFFlatCC)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVF_FLAT_CC, IvfIndexNode, knowhere::feature::NONE, faiss::IndexIVFFlatCC)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(SCANN, IvfIndexNode, knowhere::feature::MMAP, faiss::IndexScaNN)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVFPQ, IvfIndexNode, knowhere::feature::MMAP, faiss::IndexIVFPQ)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVF_PQ, IvfIndexNode, knowhere::feature::MMAP, faiss::IndexIVFPQ)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVFSQ, IvfIndexNode, knowhere::feature::MMAP,
                                              faiss::IndexIVFScalarQuantizer)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVF_SQ, IvfIndexNode, knowhere::feature::MMAP,
                                              faiss::IndexIVFScalarQuantizer)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVF_SQ8, IvfIndexNode, knowhere::feature::MMAP,
                                              faiss::IndexIVFScalarQuantizer)
KNOWHERE_MOCK_REGISTER_DENSE_FLOAT_ALL_GLOBAL(IVF_SQ_CC, IvfIndexNode, knowhere::feature::NONE,
                                              faiss::IndexIVFScalarQuantizerCC)

}  // namespace knowhere
