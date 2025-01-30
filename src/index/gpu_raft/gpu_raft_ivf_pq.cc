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

#include <vector>

#include "common/raft/proto/raft_index_kind.hpp"
#include "gpu_raft.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/index/index_node_thread_pool_wrapper.h"
#include "raft/util/cuda_rt_essentials.hpp"

namespace knowhere {
KNOWHERE_REGISTER_GLOBAL_WITH_THREAD_POOL(GPU_CUVS_IVF_PQ, GpuRaftIvfPqIndexNode, fp32,
                                          knowhere::feature::GPU_ANN_FLOAT_INDEX,
                                          []() {
                                              int count;
                                              RAFT_CUDA_TRY(cudaGetDeviceCount(&count));
                                              return count * cuda_concurrent_size_per_device;
                                          }()

);
KNOWHERE_REGISTER_GLOBAL_WITH_THREAD_POOL(GPU_IVF_PQ, GpuRaftIvfPqIndexNode, fp32,
                                          knowhere::feature::GPU_ANN_FLOAT_INDEX,
                                          []() {
                                              int count;
                                              RAFT_CUDA_TRY(cudaGetDeviceCount(&count));
                                              return count * cuda_concurrent_size_per_device;
                                          }()

);
}  // namespace knowhere
