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
#include "faiss/gpu/GpuCloner.h"
#include "faiss/index_io.h"
#include "index/flat_gpu/flat_gpu_config.h"
#include "index/gpu/gpu_res_mgr.h"
#include "io/memory_io.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/log.h"

namespace knowhere {

template <typename T>
class GpuFlatIndexNode : public IndexNode {
 public:
    GpuFlatIndexNode(const int32_t& version, const Object& object) : index_(nullptr) {
    }

    Status
    Train(const DataSetPtr dataset, const Config& cfg) override {
        const GpuFlatConfig& f_cfg = static_cast<const GpuFlatConfig&>(cfg);
        auto metric = Str2FaissMetricType(f_cfg.metric_type);
        if (!metric.has_value()) {
            LOG_KNOWHERE_WARNING_ << "metric type error, " << f_cfg.metric_type;
            return metric.error();
        }
        index_ = std::make_unique<faiss::IndexFlat>(dataset->GetDim(), metric.value());
        return Status::success;
    }

    Status
    Add(const DataSetPtr dataset, const Config& cfg) override {
        const void* x = dataset->GetTensor();
        const int64_t n = dataset->GetRows();
        try {
            index_->add(n, (const float*)x);
            // need not copy index from CPU to GPU for IDMAP
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return Status::faiss_inner_error;
        }
        return Status::success;
    }

    expected<DataSetPtr>
    Search(const DataSetPtr dataset, const Config& cfg, const BitsetView& bitset) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "index not empty, deleted old index.";
            return expected<DataSetPtr>::Err(Status::empty_index, "index not loaded");
        }

        const FlatConfig& f_cfg = static_cast<const FlatConfig&>(cfg);
        auto nq = dataset->GetRows();
        auto x = dataset->GetTensor();
        auto len = f_cfg.k * nq;
        int64_t* ids = nullptr;
        float* dis = nullptr;
        try {
            ids = new (std::nothrow) int64_t[len];
            dis = new (std::nothrow) float[len];

            ResScope rs(res_, false);
            index_->search(nq, (const float*)x, f_cfg.k, dis, ids, bitset);
        } catch (const std::exception& e) {
            std::unique_ptr<int64_t[]> auto_delete_ids(ids);
            std::unique_ptr<float[]> auto_delete_dis(dis);
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }

        return GenResultDataSet(nq, f_cfg.k, ids, dis);
    }

    expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, const Config& cfg, const BitsetView& bitset) const override {
        return Status::not_implemented;
    }

    expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const override {
        DataSetPtr results = std::make_shared<DataSet>();
        auto nq = dataset->GetRows();
        auto dim = dataset->GetDim();
        auto in_ids = dataset->GetIds();
        try {
            float* xq = new (std::nothrow) float[nq * dim];
            for (int64_t i = 0; i < nq; i++) {
                int64_t id = in_ids[i];
                index_->reconstruct(id, xq + i * dim);
            }
            return GenResultDataSet(xq);
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error: " << e.what();
            return expected<DataSetPtr>::Err(Status::faiss_inner_error, e.what());
        }
    }

    expected<DataSetPtr>
    GetIndexMeta(const Config& cfg) const override {
        return Status::not_implemented;
    }

    Status
    Serialize(BinarySet& binset) const override {
        if (!index_) {
            LOG_KNOWHERE_WARNING_ << "serilalization on empty index.";
            return Status::empty_index;
        }
        try {
            MemoryIOWriter writer;
            // Serialize() is called after Add(), at this time index_ is CPU index actually
            faiss::write_index(index_.get(), &writer);
            std::shared_ptr<uint8_t[]> data(writer.data());
            binset.Append(Type(), data, writer.tellg());
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return Status::faiss_inner_error;
        }
        return Status::success;
    }

    Status
    Deserialize(BinarySet&& binset, const Config& config) override {
        auto binary = binset.GetByName(Type());
        if (binary == nullptr) {
            LOG_KNOWHERE_ERROR_ << "Invalid binary set.";
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
        } catch (const std::exception& e) {
            LOG_KNOWHERE_WARNING_ << "faiss inner error, " << e.what();
            return Status::faiss_inner_error;
        }

        return Status::success;
    }

    Status
    DeserializeFromFile(const std::string& filename, const Config& config) override {
        LOG_KNOWHERE_ERROR_ << "GpuFlatIndex doesn't support Deserialization from file.";
        return Status::not_implemented;
    }

    static std::unique_ptr<BaseConfig>
    StaticCreateConfig() {
        return std::make_unique<GpuFlatConfig>();
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
        return index_->ntotal * index_->d * sizeof(float);
    }

    int64_t
    Count() const override {
        return index_->ntotal;
    }

    std::string
    Type() const override {
        return knowhere::IndexEnum::INDEX_FAISS_GPU_IDMAP;
    }

 private:
    mutable ResWPtr res_;
    std::unique_ptr<faiss::Index> index_;
};
// GPU_FAISS_FLAT is deprecated
}  // namespace knowhere
