/**
 * SPDX-FileCopyrightText: Copyright (c) 2023,NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef GPU_CUVS_IVF_PQ_CONFIG_H
#define GPU_CUVS_IVF_PQ_CONFIG_H

#include "common/raft/integration/raft_knowhere_config.hpp"
#include "common/raft/proto/raft_index_kind.hpp"
#include "index/ivf/ivf_config.h"
#include "knowhere/config.h"

namespace knowhere {

struct GpuRaftIvfPqConfig : public IvfPqConfig {
    CFG_BOOL cache_dataset_on_device;
    CFG_FLOAT refine_ratio;
    CFG_INT kmeans_n_iters;
    CFG_FLOAT kmeans_trainset_fraction;

    CFG_STRING codebook_kind;
    CFG_BOOL force_random_rotation;
    CFG_BOOL conservative_memory_allocation;
    CFG_STRING lut_dtype;
    CFG_STRING internal_distance_dtype;
    CFG_FLOAT preferred_shmem_carveout;

    KNOHWERE_DECLARE_CONFIG(GpuRaftIvfPqConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(cache_dataset_on_device)
            .set_default(false)
            .description("cache dataset on device for refinement")
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(refine_ratio)
            .set_default(1.0f)
            .description("search refine_ratio * k results then refine")
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(k)
            .set_default(10)
            .description("search for top k similar vector.")
            .set_range(1, 1024)  // Declared in base but limited to 1024
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(m).set_default(0).description("m").set_range(0, 65536).for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(nbits)
            .set_default(8)
            .description("nbits")
            .set_range(4, 8)  // Declared in base but limited here
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(kmeans_n_iters)
            .description("iterations to search for kmeans centers")
            .set_default(20)
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(kmeans_trainset_fraction)
            .description("fraction of data to use in kmeans building")
            .set_default(0.5)
            .for_train();

        KNOWHERE_CONFIG_DECLARE_FIELD(codebook_kind)
            .description("how PQ codebooks are generated")
            .set_default("PER_SUBSPACE")
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(force_random_rotation)
            .description("always perform random_rotation")
            .set_default(false)
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(conservative_memory_allocation)
            .description("use minimum GPU memory at the cost of speed")
            .set_default(false)
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(lut_dtype)
            .description("data type for lookup table")
            .set_default("CUDA_R_32F")
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(internal_distance_dtype)
            .description("Data type for distance storage")
            .set_default("CUDA_R_32F")
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(preferred_shmem_carveout)
            .description("preferred fraction of memory for shmem vs L1")
            .set_range(0.0f, 1.0f)
            .set_default(1.0f)
            .for_search();
    }

    Status
    CheckAndAdjust(PARAM_TYPE param_type, std::string* err_msg) override {
        if (param_type == PARAM_TYPE::TRAIN) {
            constexpr std::array<std::string_view, 3> legal_metric_list{"L2", "IP", "COSINE"};
            std::string metric = metric_type.value();
            if (std::find(legal_metric_list.begin(), legal_metric_list.end(), metric) == legal_metric_list.end()) {
                std::string msg = "metric type " + metric + " not found or not supported, supported: [L2 IP COSINE]";
                return HandleError(err_msg, msg, Status::invalid_metric_type);
            }
        }
        return Status::success;
    }
};

[[nodiscard]] inline auto
to_raft_knowhere_config(GpuRaftIvfPqConfig const& cfg) {
    auto result = raft_knowhere::raft_knowhere_config{raft_proto::raft_index_kind::ivf_pq};

    result.metric_type = cfg.metric_type.value();
    result.cache_dataset_on_device = cfg.cache_dataset_on_device.value();
    result.refine_ratio = cfg.refine_ratio.value();
    result.k = cfg.k.value();

    result.nlist = cfg.nlist;
    result.nprobe = cfg.nprobe;
    result.kmeans_n_iters = cfg.kmeans_n_iters;
    result.kmeans_trainset_fraction = cfg.kmeans_trainset_fraction;

    result.m = cfg.m;
    result.nbits = cfg.nbits;
    result.codebook_kind = cfg.codebook_kind;
    result.force_random_rotation = cfg.force_random_rotation;
    result.conservative_memory_allocation = cfg.conservative_memory_allocation;
    result.lookup_table_dtype = cfg.lut_dtype;
    result.internal_distance_dtype = cfg.internal_distance_dtype;
    result.preferred_shmem_carveout = cfg.preferred_shmem_carveout;

    return result;
}

}  // namespace knowhere

#endif /*GPU_CUVS_IVF_PQ_CONFIG_H*/
