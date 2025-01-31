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
#ifndef GPU_CUVS_CAGRA_CONFIG_H
#define GPU_CUVS_CAGRA_CONFIG_H

#include "common/raft/integration/raft_knowhere_config.hpp"
#include "common/raft/proto/raft_index_kind.hpp"
#include "index/ivf/ivf_config.h"
#include "knowhere/config.h"

namespace knowhere {
namespace {
constexpr const CFG_INT::value_type kSearchWidth = 1;
constexpr const CFG_INT::value_type kAlignFactor = 32;
constexpr const CFG_INT::value_type kItopkSize = 64;
}  // namespace

struct GpuRaftCagraConfig : public BaseConfig {
    CFG_BOOL cache_dataset_on_device;
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
    CFG_BOOL adapt_for_cpu;
    CFG_INT ef;

    KNOHWERE_DECLARE_CONFIG(GpuRaftCagraConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(cache_dataset_on_device)
            .set_default(false)
            .description("cache dataset on device for refinement")
            .for_train();
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
            .allow_empty_without_default()
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(max_queries).description("maximum batch size").set_default(0).for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(build_algo)
            .description("algorithm used to build knn graph")
            .set_default("NN_DESCENT")
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
            .allow_empty_without_default()
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
        KNOWHERE_CONFIG_DECLARE_FIELD(adapt_for_cpu)
            .description("train on GPU search on CPU")
            .set_default(false)
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(ef)
            .description("hnsw ef")
            .allow_empty_without_default()
            .set_range(1, std::numeric_limits<CFG_INT::value_type>::max())
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

        if (param_type == PARAM_TYPE::SEARCH) {
            // auto align itopk_size
            auto itopk_v = itopk_size.value_or(std::max(k.value(), kItopkSize));
            itopk_size = int32_t((itopk_v + kAlignFactor - 1) / kAlignFactor) * kAlignFactor;

            if (search_width.has_value()) {
                if (std::max(itopk_size.value(), kAlignFactor * search_width.value()) < k.value()) {
                    std::string msg = "max((itopk_size + 31)// 32, search_width) * 32< topk";
                    return HandleError(err_msg, msg, Status::out_of_range_in_json);
                }
            } else {
                search_width = std::max((k.value() - 1) / kAlignFactor + 1, kSearchWidth);
            }
        }
        return Status::success;
    }
};

[[nodiscard]] inline auto
to_raft_knowhere_config(GpuRaftCagraConfig const& cfg) {
    auto result = raft_knowhere::raft_knowhere_config{raft_proto::raft_index_kind::cagra};

    result.metric_type = cfg.metric_type.value();
    result.cache_dataset_on_device = cfg.cache_dataset_on_device.value();
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

#endif /*GPU_CUVS_CAGRA_CONFIG_H*/
