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

#include "knowhere/index/index_factory.h"

#include "knowhere/index/index_table.h"
#include "simd/hook.h"

#ifdef KNOWHERE_WITH_CUVS
#include <cuda_runtime_api.h>
#endif

namespace knowhere {

#ifdef KNOWHERE_WITH_CUVS

bool
checkGpuAvailable(const std::string& name) {
    if (name == "GPU_RAFT_BRUTE_FORCE" || name == "GPU_BRUTE_FORCE" || name == "GPU_RAFT_CAGRA" ||
        name == "GPU_CAGRA" || name == "GPU_RAFT_IVF_FLAT" || name == "GPU_IVF_FLAT" || name == "GPU_RAFT_IVF_PQ" ||
        name == "GPU_IVF_PQ") {
        int count = 0;
        auto status = cudaGetDeviceCount(&count);
        if (status != cudaSuccess) {
            LOG_KNOWHERE_INFO_ << cudaGetErrorString(status);
            return false;
        }
        if (count < 1) {
            LOG_KNOWHERE_INFO_ << "GPU not available";
            return false;
        }
    }
    return true;
}
#endif

template <typename DataType>
expected<Index<IndexNode>>
IndexFactory::Create(const std::string& name, const int32_t& version, const Object& object) {
    static_assert(KnowhereDataTypeCheck<DataType>::value == true);
    auto& func_mapping_ = MapInstance();
    auto key = GetKey<DataType>(name);
    if (func_mapping_.find(key) == func_mapping_.end()) {
        LOG_KNOWHERE_ERROR_ << "failed to find index " << key << " in factory";
        return expected<Index<IndexNode>>::Err(Status::invalid_index_error, "index not supported");
    }
    LOG_KNOWHERE_INFO_ << "use key " << key << " to create knowhere index " << name << " with version " << version;
    auto fun_map_v = (FunMapValue<Index<IndexNode>>*)(func_mapping_[key].get());

#ifdef KNOWHERE_WITH_CUVS
    if (!checkGpuAvailable(name)) {
        return expected<Index<IndexNode>>::Err(Status::cuda_runtime_error, "gpu not available");
    }
#endif
    if (name == knowhere::IndexEnum::INDEX_FAISS_SCANN && !faiss::support_pq_fast_scan) {
        LOG_KNOWHERE_ERROR_ << "SCANN index is not supported on the current CPU model";
        return expected<Index<IndexNode>>::Err(Status::invalid_index_error,
                                               "SCANN index is not supported on the current CPU model");
    }

    return fun_map_v->fun_value(version, object);
}

template <typename DataType>
const IndexFactory&
IndexFactory::Register(const std::string& name, std::function<Index<IndexNode>(const int32_t&, const Object&)> func,
                       const uint64_t features) {
    static_assert(KnowhereDataTypeCheck<DataType>::value == true);
    auto& func_mapping_ = MapInstance();
    auto key = GetKey<DataType>(name);
    assert(func_mapping_.find(key) == func_mapping_.end());
    func_mapping_[key] = std::make_unique<FunMapValue<Index<IndexNode>>>(func);
    auto& feature_mapping_ = FeatureMapInstance();
    // Index feature use the raw name
    if (feature_mapping_.find(name) == feature_mapping_.end()) {
        feature_mapping_[name] = features;
    } else {
        // All data types should have the same features; please try to avoid breaking this rule.
        feature_mapping_[name] |= features;
    }
    return *this;
}

IndexFactory&
IndexFactory::Instance() {
    static IndexFactory factory;
    return factory;
}

IndexFactory::IndexFactory() {
}
IndexFactory::FuncMap&
IndexFactory::MapInstance() {
    static FuncMap func_map;
    return func_map;
}

IndexFactory::FeatureMap&
IndexFactory::FeatureMapInstance() {
    static FeatureMap featureMap;
    return featureMap;
}

IndexFactory::GlobalIndexTable&
IndexFactory::StaticIndexTableInstance() {
    static GlobalIndexTable static_index_table;
    return static_index_table;
}

bool
IndexFactory::FeatureCheck(const std::string& name, uint64_t feature) const {
    auto& feature_mapping_ = IndexFactory::FeatureMapInstance();
    assert(feature_mapping_.find(name) != feature_mapping_.end());
    return (feature_mapping_[name] & feature) == feature;
}

const std::map<std::string, uint64_t>&
IndexFactory::GetIndexFeatures() {
    return FeatureMapInstance();
}

}  // namespace knowhere
   //
template knowhere::expected<knowhere::Index<knowhere::IndexNode>>
knowhere::IndexFactory::Create<knowhere::fp32>(const std::string&, const int32_t&, const Object&);
template knowhere::expected<knowhere::Index<knowhere::IndexNode>>
knowhere::IndexFactory::Create<knowhere::bin1>(const std::string&, const int32_t&, const Object&);
template knowhere::expected<knowhere::Index<knowhere::IndexNode>>
knowhere::IndexFactory::Create<knowhere::fp16>(const std::string&, const int32_t&, const Object&);
template knowhere::expected<knowhere::Index<knowhere::IndexNode>>
knowhere::IndexFactory::Create<knowhere::bf16>(const std::string&, const int32_t&, const Object&);
template knowhere::expected<knowhere::Index<knowhere::IndexNode>>
knowhere::IndexFactory::Create<knowhere::int8>(const std::string&, const int32_t&, const Object&);
template const knowhere::IndexFactory&
knowhere::IndexFactory::Register<knowhere::fp32>(
    const std::string&, std::function<knowhere::Index<knowhere::IndexNode>(const int32_t&, const Object&)>,
    const uint64_t);
template const knowhere::IndexFactory&
knowhere::IndexFactory::Register<knowhere::bin1>(
    const std::string&, std::function<knowhere::Index<knowhere::IndexNode>(const int32_t&, const Object&)>,
    const uint64_t);
template const knowhere::IndexFactory&
knowhere::IndexFactory::Register<knowhere::fp16>(
    const std::string&, std::function<knowhere::Index<knowhere::IndexNode>(const int32_t&, const Object&)>,
    const uint64_t);
template const knowhere::IndexFactory&
knowhere::IndexFactory::Register<knowhere::bf16>(
    const std::string&, std::function<knowhere::Index<knowhere::IndexNode>(const int32_t&, const Object&)>,
    const uint64_t);
template const knowhere::IndexFactory&
knowhere::IndexFactory::Register<knowhere::int8>(
    const std::string&, std::function<knowhere::Index<knowhere::IndexNode>(const int32_t&, const Object&)>,
    const uint64_t);
