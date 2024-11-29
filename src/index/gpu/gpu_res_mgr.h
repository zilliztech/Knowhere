// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#pragma once

#include <faiss/gpu/StandardGpuResources.h>
#ifdef KNOWHERE_WITH_CUVS
#include <rmm/cuda_device.hpp>
#include <rmm/mr/device/device_memory_resource.hpp>
#include <rmm/mr/device/per_device_resource.hpp>
#include <rmm/mr/device/pool_memory_resource.hpp>
#include <vector>
#endif

#include <memory>
#include <mutex>
#include <utility>

#include "knowhere/comp/blocking_queue.h"
#include "knowhere/log.h"

namespace knowhere {

constexpr int64_t MB = 1LL << 20;

struct Resource {
    Resource(int64_t gpu_id, faiss::gpu::StandardGpuResources* r) : faiss_res_(r), gpu_id_(gpu_id) {
        static int64_t global_id = 0;
        id_ = global_id++;
    }

    std::unique_ptr<faiss::gpu::StandardGpuResources> faiss_res_;
    int64_t id_;
    int64_t gpu_id_;
    std::mutex mutex_;
};
using ResPtr = std::shared_ptr<Resource>;
using ResWPtr = std::weak_ptr<Resource>;

struct GPUParams {
    int64_t tmp_mem_sz_ = 256 * MB;
    int64_t pin_mem_sz_ = 256 * MB;
    int64_t res_num_ = 2;

    GPUParams() {
    }

    GPUParams(int64_t res_num) : res_num_(res_num) {
    }
};

class GPUResMgr {
 public:
    friend class ResScope;
    using ResBQ = BlockingQueue<ResPtr>;

 public:
    static GPUResMgr&
    GetInstance() {
        static GPUResMgr instance;
        return instance;
    }

    void
    InitDevice(const int64_t gpu_id, const GPUParams& gpu_params) {
        // check gpu device validation
        faiss::gpu::setCurrentDevice(gpu_id);

        gpu_id_ = gpu_id;
        gpu_params_.res_num_ = gpu_params.res_num_;
        gpu_params_.tmp_mem_sz_ = gpu_params.tmp_mem_sz_;
        gpu_params_.pin_mem_sz_ = gpu_params.pin_mem_sz_;

        LOG_KNOWHERE_DEBUG_ << "InitDevice gpu_id " << gpu_id_ << ", resource count " << gpu_params_.res_num_
                            << ", tmp_mem_sz " << gpu_params_.tmp_mem_sz_ / MB << "MB, pin_mem_sz "
                            << gpu_params_.pin_mem_sz_ / MB << "MB";
    }

    void
    Init() {
        if (!init_) {
            // double-check for thread safe
            std::lock_guard<std::mutex> lock(init_mutex_);
            if (!init_) {
                for (int64_t i = 0; i < gpu_params_.res_num_; ++i) {
                    auto gpu_res = new faiss::gpu::StandardGpuResources();
                    auto res = std::make_shared<Resource>(gpu_id_, gpu_res);

                    cudaStream_t s;
                    CUDA_VERIFY(cudaStreamCreate(&s));
                    gpu_res->setDefaultStream(gpu_id_, s);
                    gpu_res->setTempMemory(gpu_params_.tmp_mem_sz_);
                    // need not set pinned memory by now

                    res_bq_.Put(res);
                }
                LOG_KNOWHERE_DEBUG_ << "Init gpu_id " << gpu_id_ << ", resource count " << res_bq_.Size()
                                    << ", tmp_mem_sz " << gpu_params_.tmp_mem_sz_ / MB << "MB";
                init_ = true;
            }
        }
    }

    // Free GPU resource, avoid cudaGetDevice error when deallocate.
    // This func should be invoked before main return
    void
    Free() {
        while (!res_bq_.Empty()) {
            res_bq_.Take();
        }
        init_ = false;
    }

    ResPtr
    GetRes() {
        // Generally Init() should be called separately,
        // here is for supporting python test
        Init();
        auto res = res_bq_.Take();
        return res;
    }

    void
    PutRes(const ResPtr& res) {
        res_bq_.Put(res);
    }

 protected:
    bool init_ = false;
    std::mutex init_mutex_;

    int64_t gpu_id_ = 0;
    GPUParams gpu_params_;
    ResBQ res_bq_;
};

class ResScope {
 public:
    ResScope(ResPtr& res, const bool renew) : res_(res), renew_(renew) {
        res_->mutex_.lock();
    }

    ResScope(ResWPtr& res, const bool renew) : res_(res.lock()), renew_(renew) {
        res_->mutex_.lock();
    }

    ~ResScope() {
        if (renew_) {
            GPUResMgr::GetInstance().PutRes(res_);
        }
        res_->mutex_.unlock();
    }

 private:
    ResPtr res_;  // hold resource until deconstruct
    bool renew_;
};

}  // namespace knowhere
