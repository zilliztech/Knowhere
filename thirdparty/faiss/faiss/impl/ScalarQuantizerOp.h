/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <faiss/impl/ScalarQuantizer.h>

namespace faiss {

void train_Uniform(
        ScalarQuantizer::RangeStat rs,
        float rs_arg,
        idx_t n,
        int k,
        const float* x,
        std::vector<float>& trained);

void train_NonUniform(
        ScalarQuantizer::RangeStat rs,
        float rs_arg,
        idx_t n,
        int d,
        int k,
        const float* x,
        std::vector<float>& trained);

} // namespace faiss
