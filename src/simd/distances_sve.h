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

#include <arm_sve.h>

#include <cstdint>
#include <cstdio>
#if defined(__ARM_FEATURE_SVE)
namespace faiss {

float
fvec_L2sqr_sve(const float* x, const float* y, size_t d);

float
fvec_L1_sve(const float* x, const float* y, size_t d);

float
fvec_Linf_sve(const float* x, const float* y, size_t d);

float
fvec_norm_L2sqr_sve(const float* x, size_t d);

void
fvec_madd_sve(size_t n, const float* a, float bf, const float* b, float* c);

int
fvec_madd_and_argmin_sve(size_t n, const float* a, float bf, const float* b, float* c);

int32_t
ivec_L2sqr_sve(const int8_t* x, const int8_t* y, size_t d);

void
fvec_L2sqr_batch_4_sve(const float* x, const float* y0, const float* y1, const float* y2, const float* y3,
                       const size_t d, float& dis0, float& dis1, float& dis2, float& dis3);

}  // namespace faiss
#endif
