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

#include "knowhere/comp/knowhere_config.h"

#include <string>

#ifdef KNOWHERE_WITH_DISKANN
#include "diskann/aio_context_pool.h"
#endif
#include "faiss/Clustering.h"
#include "faiss/utils/distances.h"
#include "knowhere/comp/thread_pool.h"
#include "knowhere/log.h"
#ifdef KNOWHERE_WITH_GPU
#include "index/gpu/gpu_res_mgr.h"
#endif
#ifdef KNOWHERE_WITH_CUVS
#include "common/raft/integration/raft_initialization.hpp"
#include "cuda_runtime_api.h"
#endif
#include "simd/hook.h"

namespace knowhere {

void
KnowhereConfig::ShowVersion() {
#define XSTR(x) STR(x)
#define STR(x) #x

    std::string msg = "Knowhere Version: ";

#ifdef KNOWHERE_VERSION
    msg = msg + XSTR(KNOWHERE_VERSION);
#ifdef KNOWHERE_WITH_CUVS
    msg = msg + "-gpu";
#endif
#else
    msg = msg + " unknown";
#endif

#ifndef NDEBUG
    msg = msg + " (DEBUG)";
#endif

    LOG_KNOWHERE_INFO_ << msg;
}

std::string
KnowhereConfig::SetSimdType(const SimdType simd_type) {
#ifdef __x86_64__
    if (simd_type == SimdType::AUTO) {
        faiss::use_avx512 = true;
        faiss::use_avx2 = true;
        faiss::use_sse4_2 = true;
        LOG_KNOWHERE_INFO_ << "FAISS expect simdType::AUTO";
    } else if (simd_type == SimdType::AVX512) {
        faiss::use_avx512 = true;
        faiss::use_avx2 = true;
        faiss::use_sse4_2 = true;
        LOG_KNOWHERE_INFO_ << "FAISS expect simdType::AVX512";
    } else if (simd_type == SimdType::AVX2) {
        faiss::use_avx512 = false;
        faiss::use_avx2 = true;
        faiss::use_sse4_2 = true;
        LOG_KNOWHERE_INFO_ << "FAISS expect simdType::AVX2";
    } else if (simd_type == SimdType::SSE4_2) {
        faiss::use_avx512 = false;
        faiss::use_avx2 = false;
        faiss::use_sse4_2 = true;
        LOG_KNOWHERE_INFO_ << "FAISS expect simdType::SSE4_2";
    } else if (simd_type == SimdType::GENERIC) {
        faiss::use_avx512 = false;
        faiss::use_avx2 = false;
        faiss::use_sse4_2 = false;
        LOG_KNOWHERE_INFO_ << "FAISS expect simdType::GENERIC";
    }
#endif
    std::string simd_str;
    faiss::fvec_hook(simd_str);
    LOG_KNOWHERE_INFO_ << "FAISS hook " << simd_str;
    return simd_str;
}

void
KnowhereConfig::EnablePatchForComputeFP32AsBF16() {
    LOG_KNOWHERE_INFO_ << "Enable patch for compute fp32 as bf16";
    faiss::enable_patch_for_fp32_bf16();
}

void
KnowhereConfig::DisablePatchForComputeFP32AsBF16() {
    LOG_KNOWHERE_INFO_ << "Disable patch for compute fp32 as bf16";
    faiss::disable_patch_for_fp32_bf16();
}

void
KnowhereConfig::SetBlasThreshold(const int64_t use_blas_threshold) {
    LOG_KNOWHERE_INFO_ << "Set faiss::distance_compute_blas_threshold to " << use_blas_threshold;
    faiss::distance_compute_blas_threshold = static_cast<int>(use_blas_threshold);
}

int64_t
KnowhereConfig::GetBlasThreshold() {
    return faiss::distance_compute_blas_threshold;
}

void
KnowhereConfig::SetEarlyStopThreshold(const double early_stop_threshold) {
    LOG_KNOWHERE_INFO_ << "Set faiss::early_stop_threshold to " << early_stop_threshold;
    faiss::early_stop_threshold = early_stop_threshold;
}

double
KnowhereConfig::GetEarlyStopThreshold() {
    return faiss::early_stop_threshold;
}

void
KnowhereConfig::SetClusteringType(const ClusteringType clustering_type) {
    LOG_KNOWHERE_INFO_ << "Set faiss::clustering_type to " << clustering_type;
    switch (clustering_type) {
        case ClusteringType::K_MEANS:
        default:
            faiss::clustering_type = faiss::ClusteringType::K_MEANS;
            break;
        case ClusteringType::K_MEANS_PLUS_PLUS:
            faiss::clustering_type = faiss::ClusteringType::K_MEANS_PLUS_PLUS;
            break;
    }
}

bool
KnowhereConfig::SetAioContextPool(size_t num_ctx) {
#ifdef KNOWHERE_WITH_DISKANN
    return AioContextPool::InitGlobalAioPool(num_ctx, default_max_events);
#endif
    return true;
}

void
KnowhereConfig::SetBuildThreadPoolSize(size_t num_threads) {
    knowhere::ThreadPool::SetGlobalBuildThreadPoolSize(num_threads);
}

size_t
KnowhereConfig::GetBuildThreadPoolSize() {
    return knowhere::ThreadPool::GetGlobalBuildThreadPoolSize();
}

void
KnowhereConfig::SetSearchThreadPoolSize(size_t num_threads) {
    knowhere::ThreadPool::SetGlobalSearchThreadPoolSize(num_threads);
}

size_t
KnowhereConfig::GetSearchThreadPoolSize() {
    return knowhere::ThreadPool::GetGlobalSearchThreadPoolSize();
}

void
KnowhereConfig::InitGPUResource(int64_t gpu_id, int64_t res_num) {
#ifdef KNOWHERE_WITH_GPU
    LOG_KNOWHERE_INFO_ << "init GPU resource for gpu id " << gpu_id << ", resource num " << res_num;

    knowhere::GPUParams gpu_params(res_num);
    knowhere::GPUResMgr::GetInstance().InitDevice(gpu_id, gpu_params);
    knowhere::GPUResMgr::GetInstance().Init();
#endif
}

void
KnowhereConfig::FreeGPUResource() {
#ifdef KNOWHERE_WITH_GPU
    LOG_KNOWHERE_INFO_ << "free GPU resource";
    knowhere::GPUResMgr::GetInstance().Free();
#endif
}

void
KnowhereConfig::SetRaftMemPool(size_t init_size, size_t max_size) {
#ifdef KNOWHERE_WITH_CUVS
    int count = 0;
    auto status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess) {
        LOG_KNOWHERE_INFO_ << cudaGetErrorString(status);
        return;
    }
    if (count < 1) {
        LOG_KNOWHERE_INFO_ << "GPU not available";
        return;
    }

    auto config = raft_knowhere::raft_configuration{};
    config.init_mem_pool_size_mb = init_size;
    config.max_mem_pool_size_mb = max_size;
    // This should probably be a separate configuration option, but fine for now
    config.max_workspace_size_mb = max_size;
    raft_knowhere::initialize_raft(config);
#endif
}

void
KnowhereConfig::SetRaftMemPool() {
    // Overload for default values
#ifdef KNOWHERE_WITH_CUVS
    auto config = raft_knowhere::raft_configuration{};
    raft_knowhere::initialize_raft(config);
#endif
}

}  // namespace knowhere
