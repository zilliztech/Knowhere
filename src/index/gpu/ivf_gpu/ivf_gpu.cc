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
#include "faiss/IndexFlat.h"
#include "faiss/IndexIVFFlat.h"
#include "faiss/IndexIVFPQ.h"
#include "faiss/IndexReplicas.h"
#include "faiss/IndexScalarQuantizer.h"
#include "faiss/gpu/GpuCloner.h"
#include "faiss/gpu/GpuIndexIVF.h"
#include "faiss/gpu/GpuIndexIVFFlat.h"
#include "faiss/gpu/GpuIndexIVFPQ.h"
#include "faiss/gpu/GpuIndexIVFScalarQuantizer.h"
#include "faiss/index_io.h"
#include "index/gpu/gpu_res_mgr.h"
#include "index/ivf_gpu/ivf_gpu_config.h"
#include "io/memory_io.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/log.h"

namespace knowhere {

template <typename T>
struct KnowhereConfigType {};

template <>
struct KnowhereConfigType<faiss::IndexIVFFlat> {
    typedef GpuIvfFlatConfig Type;
};
template <>
struct KnowhereConfigType<faiss::IndexIVFPQ> {
    typedef GpuIvfPqConfig Type;
};
template <>
struct KnowhereConfigType<faiss::IndexIVFScalarQuantizer> {
    typedef GpuIvfSqConfig Type;
};

template <typename T>
class GpuIvfIndexNode : public IndexNode {
 public:
    GpuIvfIndexNode(const int32_t& version, const Object& object) : index_(nullptr) {
        static_assert(std::is_same<T, faiss::IndexIVFFlat>::value || std::is_same<T, faiss::IndexIVFPQ>::value ||
                      std::is_same<T, faiss::IndexIVFScalarQuantizer>::value);
    }

    Status
    Train(const DataSetPtr dataset, const Config& cfg) override {
        if (index_ && index_->is_trained) {
            LOG_KNOWHERE_WARNING_ << "index is already trained";
            return Status::index_already_trained;
        }

        auto rows = dataset->GetRows();
        auto tensor = dataset->GetTensor();
        auto dim = dataset->GetDim();
        auto ivf_gpu_cfg = static_cast<const typename KnowhereConfigType<T>::Type&>(cfg);

        auto metric = Str2FaissMetricType(ivf_gpu_cfg.metric_type);
        if (!metric.has_value()) {
            LOG_KNOWHERE_ERROR_ << "unsupported metric type: " << ivf_gpu_cfg.metric_type;
            return metric.error();
        }

        std::unique_ptr<faiss::Index> index;
        try {
            auto gpu_res = GPUResMgr::GetInstance().GetRes();
            ResScope rs(gpu_res, true);

            if constexpr (std::is_same<T, faiss::IndexIVFFlat>::value) {
                faiss::gpu::GpuIndexIVFFlatConfig f_cfg;
                f_cfg.device = static_cast<int32_t>(gpu_res->gpu_id_);
                index = std::make_unique<faiss::gpu::GpuIndexIVFFlat>(gpu_res->faiss_res_.get(), dim, ivf_gpu_cfg.nlist,
                                                                      metric.value(), f_cfg);
            }
            if constexpr (std::is_same<T, faiss::IndexIVFPQ>::value) {
                faiss::gpu::GpuIndexIVFPQConfig f_cfg;
                f_cfg.device = static_cast<int32_t>(gpu_res->gpu_id_);
                index = std::make_unique<faiss::gpu::GpuIndexIVFPQ>(gpu_res->faiss_res_.get(), dim, ivf_gpu_cfg.nlist,
                                                                    ivf_gpu_cfg.m, ivf_gpu_cfg.nbits, metric.value(),
                                                                    f_cfg);
            }
            if constexpr (std::is_same<T, faiss::IndexIVFScalarQuantizer>::value) {
                faiss::gpu::GpuIndexIVFScalarQuantizerConfig f_cfg;
                f_cfg.device = static_cast<int32_t>(gpu_res->gpu_id_);
                index = std::make_unique<faiss::gpu::GpuIndexIVFScalarQuantizer>(
                    gpu_res->faiss_res_.get(), dim, ivf_gpu_cfg.nlist, faiss::QuantizerType::QT_8bit, metric.value(),
                    true, f_cfg);
            }
            index->train(rows, reinterpret_cast<const float*>(tensor));
            res_ = gpu_res;
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return Status::faiss_inner_error;
        }
        index_ = std::move(index);
        return Status::success;
    }

    Status
    Add(const DataSetPtr dataset, const Config& cfg) override {
        if (!index_) {
            LOG_KNOWHERE_ERROR_ << "Can not add data to empty GpuIvfIndex.";
            return Status::empty_index;
        }
        if (!index_->is_trained) {
            LOG_KNOWHERE_ERROR_ << "Can not add data to not trained GpuIvfIndex.";
            return Status::index_not_trained;
        }
        auto rows = dataset->GetRows();
        auto tensor = dataset->GetTensor();
        try {
            ResScope rs(res_, false);
            index_->add(rows, (const float*)tensor);
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return Status::faiss_inner_error;
        }
        return Status::success;
    }

    expected<DataSetPtr>
    Search(const DataSetPtr dataset, const Config& cfg, const BitsetView& bitset) const override {
        auto ivf_gpu_cfg = static_cast<const typename KnowhereConfigType<T>::Type&>(cfg);

        constexpr int64_t block_size = 2048;
        auto rows = dataset->GetRows();
        auto k = ivf_gpu_cfg.k;
        auto tensor = dataset->GetTensor();
        auto dim = dataset->GetDim();
        float* dis = new (std::nothrow) float[rows * k];
        int64_t* ids = new (std::nothrow) int64_t[rows * k];
        try {
            ResScope rs(res_, false);
            auto gpu_index = dynamic_cast<faiss::gpu::GpuIndexIVF*>(index_.get());
            for (int i = 0; i < rows; i += block_size) {
                int64_t search_size = (rows - i > block_size) ? block_size : (rows - i);
                gpu_index->search_thread_safe(search_size, reinterpret_cast<const float*>(tensor) + i * dim, k,
                                              ivf_gpu_cfg.nprobe, dis + i * k, ids + i * k, bitset);
            }
        } catch (std::exception& e) {
            std::unique_ptr<float> auto_delete_dis(dis);
            std::unique_ptr<int64_t> auto_delete_ids(ids);
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }

        return GenResultDataSet(rows, ivf_gpu_cfg.k, ids, dis);
    }

    expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, const Config& cfg, const BitsetView& bitset) const override {
        return Status::not_implemented;
    }

    expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const override {
        return Status::not_implemented;
    }

    expected<DataSetPtr>
    GetIndexMeta(const Config& cfg) const override {
        return Status::not_implemented;
    }

    Status
    Serialize(BinarySet& binset) const override {
        if (!index_) {
            LOG_KNOWHERE_ERROR_ << "Can not serialize empty GpuIvfIndex.";
            return Status::empty_index;
        }
        if (!index_->is_trained) {
            LOG_KNOWHERE_ERROR_ << "Can not serialize not trained GpuIvfIndex.";
            return Status::index_not_trained;
        }

        try {
            MemoryIOWriter writer;
            {
                faiss::Index* host_index = faiss::gpu::index_gpu_to_cpu(index_.get());
                faiss::write_index(host_index, &writer);
                delete host_index;
            }
            std::shared_ptr<uint8_t[]> data(writer.data());
            binset.Append(Type(), data, writer.tellg());
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return Status::faiss_inner_error;
        }

        return Status::success;
    }

    Status
    Deserialize(BinarySet&& binset, const Config& config) override {
        auto binary = binset.GetByName(Type());
        if (binary == nullptr) {
            LOG_KNOWHERE_ERROR_ << "invalid binary set.";
            return Status::invalid_binary_set;
        }
        MemoryIOReader reader(binary->data.get(), binary->size);
        try {
            std::unique_ptr<faiss::Index> index(faiss::read_index(&reader));
            auto gpu_res = GPUResMgr::GetInstance().GetRes();
            ResScope rs(gpu_res, true);
            auto gpu_index = faiss::gpu::index_cpu_to_gpu(gpu_res->faiss_res_.get(), gpu_res->gpu_id_, index.get());
            index_.reset(gpu_index);
            res_ = gpu_res;
        } catch (std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return Status::faiss_inner_error;
        }
        return Status::success;
    }

    Status
    DeserializeFromFile(const std::string& filename, const Config& config) override {
        LOG_KNOWHERE_ERROR_ << "GpuIvfIndex doesn't support Deserialization from file.";
        return Status::not_implemented;
    }

    static std::unique_ptr<BaseConfig>
    StaticCreateConfig() {
        return std::make_unique<typename KnowhereConfigType<T>::Type>();
    }

    std::unique_ptr<BaseConfig>
    CreateConfig() const override {
        return StaticCreateConfig();
    }

    int64_t
    Dim() const override {
        if (index_) {
            return index_->d;
        }
        return 0;
    }

    int64_t
    Size() const override {
        return 0;
    }

    int64_t
    Count() const override {
        if (index_) {
            return index_->ntotal;
        }
        return 0;
    }

    std::string
    Type() const override {
        if constexpr (std::is_same<faiss::IndexIVFFlat, T>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_GPU_IVFFLAT;
        }
        if constexpr (std::is_same<faiss::IndexIVFPQ, T>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_GPU_IVFPQ;
        }
        if constexpr (std::is_same<faiss::IndexIVFScalarQuantizer, T>::value) {
            return knowhere::IndexEnum::INDEX_FAISS_GPU_IVFSQ8;
        }
    }

 private:
    mutable ResWPtr res_;
    std::unique_ptr<faiss::Index> index_;
};
// GPU_FAISS_IVF_FLAT/GPU_FAISS_IVF_PQ/GPU_FAISS_IVF_SQ8 is deprecated
}  // namespace knowhere
