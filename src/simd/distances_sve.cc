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

#include "distances_sve.h"

#include <arm_sve.h>

#include <cmath>

#include "faiss/impl/platform_macros.h"
#if defined(__ARM_FEATURE_SVE)
namespace faiss {

float
fvec_L2sqr_sve(const float* x, const float* y, size_t d) {
    svfloat32_t sum = svdup_f32(0.0f);
    size_t i = 0;

    svbool_t pg = svptrue_b32();

    while (i < d) {
        if (d - i < svcntw())
            pg = svwhilelt_b32(i, d);

        svfloat32_t a = svld1_f32(pg, x + i);
        svfloat32_t b = svld1_f32(pg, y + i);
        svfloat32_t diff = svsub_f32_m(pg, a, b);
        sum = svmla_f32_m(pg, sum, diff, diff);
        i += svcntw();
    }

    return svaddv_f32(svptrue_b32(), sum);
}

float
fvec_L1_sve(const float* x, const float* y, size_t d) {
    svfloat32_t sum = svdup_f32(0.0f);
    size_t i = 0;

    svbool_t pg = svptrue_b32();

    while (i < d) {
        if (d - i < svcntw())
            pg = svwhilelt_b32(i, d);

        svfloat32_t a = svld1_f32(pg, x + i);
        svfloat32_t b = svld1_f32(pg, y + i);
        svfloat32_t diff = svabs_f32_x(pg, svsub_f32_m(pg, a, b));
        sum = svadd_f32_m(pg, sum, diff);
        i += svcntw();
    }

    return svaddv_f32(svptrue_b32(), sum);
}

float
fvec_Linf_sve(const float* x, const float* y, size_t d) {
    svfloat32_t max_val = svdup_f32(0.0f);
    size_t i = 0;

    svbool_t pg = svptrue_b32();

    while (i < d) {
        if (d - i < svcntw())
            pg = svwhilelt_b32(i, d);

        svfloat32_t a = svld1_f32(pg, x + i);
        svfloat32_t b = svld1_f32(pg, y + i);
        svfloat32_t diff = svabs_f32_x(pg, svsub_f32_m(pg, a, b));
        max_val = svmax_f32_m(pg, max_val, diff);
        i += svcntw();
    }

    return svmaxv_f32(svptrue_b32(), max_val);
}

float
fvec_norm_L2sqr_sve(const float* x, size_t d) {
    svfloat32_t sum = svdup_f32(0.0f);
    size_t i = 0;

    svbool_t pg = svptrue_b32();

    while (i < d) {
        if (d - i < svcntw())
            pg = svwhilelt_b32(i, d);

        svfloat32_t a = svld1_f32(pg, x + i);
        sum = svmla_f32_m(pg, sum, a, a);
        i += svcntw();
    }

    return svaddv_f32(svptrue_b32(), sum);
}

void
fvec_madd_sve(size_t n, const float* a, float bf, const float* b, float* c) {
    size_t i = 0;
    svfloat32_t bf_vec = svdup_f32(bf);

    svbool_t pg = svptrue_b32();

    while (i < n) {
        if (n - i < svcntw())
            pg = svwhilelt_b32(i, n);

        svfloat32_t a_vec = svld1_f32(pg, a + i);
        svfloat32_t b_vec = svld1_f32(pg, b + i);
        svfloat32_t c_vec = svmla_f32_m(pg, a_vec, b_vec, bf_vec);
        svst1_f32(pg, c + i, c_vec);
        i += svcntw();
    }
}

int
fvec_madd_and_argmin_sve(size_t n, const float* a, float bf, const float* b, float* c) {
    size_t i = 0;
    svfloat32_t min_val = svdup_f32(INFINITY);
    svuint32_t min_idx = svdup_u32(0);
    svuint32_t idx_base = svindex_u32(0, 1);

    svfloat32_t bf_vec = svdup_f32(bf);
    svbool_t pg = svptrue_b32();

    while (i < n) {
        if (n - i < svcntw())
            pg = svwhilelt_b32(i, n);

        svuint32_t idx = svadd_u32_z(pg, idx_base, svdup_u32(i));
        svfloat32_t a_vec = svld1_f32(pg, a + i);
        svfloat32_t b_vec = svld1_f32(pg, b + i);
        svfloat32_t c_vec = svmla_f32_m(pg, a_vec, b_vec, bf_vec);
        svst1_f32(pg, c + i, c_vec);

        svbool_t cmp = svcmplt(pg, c_vec, min_val);
        min_val = svsel_f32(cmp, c_vec, min_val);
        min_idx = svsel_u32(cmp, idx, min_idx);

        i += svcntw();
    }

    float min_value = svminv_f32(svptrue_b32(), min_val);
    svbool_t pg_min = svcmpeq(svptrue_b32(), min_val, svdup_f32(min_value));
    uint32_t min_index = svlastb_u32(pg_min, min_idx);

    return static_cast<int>(min_index);
}

void
fvec_L2sqr_batch_4_sve(const float* x, const float* y0, const float* y1, const float* y2, const float* y3,
                       const size_t d, float& dis0, float& dis1, float& dis2, float& dis3) {
    float d0 = 0;
    float d1 = 0;
    float d2 = 0;
    float d3 = 0;

    for (size_t i = 0; i < d; ++i) {
        const float q0 = x[i] - y0[i];
        const float q1 = x[i] - y1[i];
        const float q2 = x[i] - y2[i];
        const float q3 = x[i] - y3[i];
        d0 += q0 * q0;
        d1 += q1 * q1;
        d2 += q2 * q2;
        d3 += q3 * q3;
    }

    dis0 = d0;
    dis1 = d1;
    dis2 = d2;
    dis3 = d3;
}

}  // namespace faiss

#endif
