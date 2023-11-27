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
#ifndef GPU_RAFT_CAGRA_CONFIG_H
#define GPU_RAFT_CAGRA_CONFIG_H

#include "common/raft/integration/raft_knowhere_config.hpp"
#include "common/raft/proto/raft_index_kind.hpp"
#include "index/ivf/ivf_config.h"
#include "knowhere/config.h"

namespace knowhere {

struct GpuRaftCagraConfig : public BaseConfig {
    CFG_FLOAT refine_ratio;
    CFG_INT intermediate_graph_degree;
    CFG_INT graph_degree;
    CFG_INT itopk_size;
    CFG_INT max_queries;
    CFG_STRING build_algo;
    CFG_STRING search_algo;
    CFG_INT team_size;
    CFG_INT search_width;
    CFG_INT min_iterations;
    CFG_INT max_iterations;
    CFG_INT thread_block_size;
    CFG_STRING hashmap_mode;
    CFG_INT hashmap_min_bitlen;
    CFG_FLOAT hashmap_max_fill_rate;
    CFG_INT nn_descent_niter;

    KNOHWERE_DECLARE_CONFIG(GpuRaftCagraConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(refine_ratio)
            .set_default(1.0f)
            .description("search refine_ratio * k results then refine")
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(intermediate_graph_degree)
            .description("degree of intermediate knn graph")
            .set_default(128)
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(graph_degree).description("degree of knn graph").set_default(64).for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(itopk_size)
            .description("intermediate results retained during search")
            .set_default(64)
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(max_queries).description("maximum batch size").set_default(0).for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(build_algo)
            .description("algorithm used to build knn graph")
            .set_default("IVF_PQ")
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(search_algo)
            .description("algorithm used for search")
            .set_default("AUTO")
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(team_size)
            .description("threads used to calculate single distance")
            .set_default(0)
            .set_range(0, 32)
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(search_width)
            .description("nodes to select as starting point in each iteration")
            .set_default(1)
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(min_iterations)
            .description("minimum number of search iterations")
            .set_default(0)
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(max_iterations)
            .description("maximum number of search iterations")
            .set_default(0)
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(thread_block_size).description("threads per block").set_default(0).for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(hashmap_mode).description("hashmap mode").set_default("AUTO").for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(hashmap_min_bitlen)
            .description("minimum bit length of hashmap")
            .set_default(0)
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(hashmap_max_fill_rate)
            .description("minimum bit length of hashmap")
            .set_default(0.5f)
            .set_range(0.1f, 0.9f)
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(nn_descent_niter)
            .description("number of iterations for NN descent")
            .set_default(20)
            .for_train();
    }
};

[[nodiscard]] inline auto
to_raft_knowhere_config(GpuRaftCagraConfig const& cfg) {
    auto result = raft_knowhere::raft_knowhere_config{raft_proto::raft_index_kind::cagra};

    result.metric_type = cfg.metric_type.value();
    result.refine_ratio = cfg.refine_ratio.value();
    result.k = cfg.k.value();

    result.intermediate_graph_degree = cfg.intermediate_graph_degree;
    result.graph_degree = cfg.graph_degree;
    result.itopk_size = cfg.itopk_size;
    result.max_queries = cfg.max_queries;
    result.build_algo = cfg.build_algo;
    result.search_algo = cfg.search_algo;
    result.team_size = cfg.team_size;
    result.search_width = cfg.search_width;
    result.min_iterations = cfg.min_iterations;
    result.max_iterations = cfg.max_iterations;
    result.thread_block_size = cfg.thread_block_size;
    result.hashmap_mode = cfg.hashmap_mode;
    result.hashmap_min_bitlen = cfg.hashmap_min_bitlen;
    result.hashmap_max_fill_rate = cfg.hashmap_max_fill_rate;
    result.nn_descent_niter = cfg.nn_descent_niter;

    return result;
}

}  // namespace knowhere

#endif /*GPU_RAFT_CAGRA_CONFIG_H*/
