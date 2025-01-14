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

#ifndef SPARSE_INVERTED_INDEX_CONFIG_H
#define SPARSE_INVERTED_INDEX_CONFIG_H

#include "knowhere/comp/index_param.h"
#include "knowhere/config.h"

namespace knowhere {

class SparseInvertedIndexConfig : public BaseConfig {
 public:
    CFG_FLOAT drop_ratio_build;
    CFG_FLOAT drop_ratio_search;
    CFG_INT refine_factor;
    CFG_FLOAT wand_bm25_max_score_ratio;
    CFG_STRING inverted_index_algo;
    CFG_INT blockmax_block_size;
    KNOHWERE_DECLARE_CONFIG(SparseInvertedIndexConfig) {
        // NOTE: drop_ratio_build has been deprecated, it won't change anything
        KNOWHERE_CONFIG_DECLARE_FIELD(drop_ratio_build)
            .description("drop ratio for build")
            .set_default(0.0f)
            .set_range(0.0f, 1.0f, true, false)
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(drop_ratio_search)
            .description("drop ratio for search")
            .set_default(0.0f)
            .set_range(0.0f, 1.0f, true, false)
            .for_search()
            .for_range_search()
            .for_iterator();
        KNOWHERE_CONFIG_DECLARE_FIELD(refine_factor)
            .description("refine factor")
            .set_default(10)
            .for_search()
            .for_range_search();
        /**
         * The term frequency part of score of BM25 is:
         * tf * (k1 + 1) / (tf + k1 * (1 - b + b * (doc_len / avgdl)))
         * WAND algorithm uses the max score of each dim for pruning, which is
         * precomputed and cached in our implementation. The cached max score
         * is actually not equal to the actual max score. Instead, it is a
         * scaled one based on the wand_bm25_max_score_ratio.
         * We should use different scale strategy for different reasons.
         * 1. As more documents being added to the collection, avgdl could
         *    be changed. Re-computing such score for each segment is
         *    expensive. To avoid this, we should upscale the actual max score
         *    by a ratio greater than 1.0 to compensate for avgdl changes.
         *    This will make the cached max score larger than the actual max
         *    score, so that it makes the filtering less aggressive, but
         *    guarantees the correctness.
         * 2. In WAND searching process, we use the sum of the max scores to
         *    filter the candidate vectors. If the sum is smaller than the
         *    threshold, skip current vector. If approximate searching is
         *    accepted, we can make the skipping more aggressive by downscaling
         *    the max score with a ratio less than 1.0. Since the possibility
         *    that the max score of all dims in the query appears on the same
         *    vector is relatively small, it won't lead to a sharp decline in
         *    the recall rate within a certain range.
         */
        KNOWHERE_CONFIG_DECLARE_FIELD(wand_bm25_max_score_ratio)
            .set_range(0.0, 1.3)
            .set_default(1.05)
            .description("ratio to upscale/downscale the max score of each dimension")
            .for_train_and_search()
            .for_deserialize()
            .for_deserialize_from_file();
        KNOWHERE_CONFIG_DECLARE_FIELD(inverted_index_algo)
            .description("inverted index algorithm")
            .set_default("DAAT_MAXSCORE")
            .for_train_and_search()
            .for_deserialize()
            .for_deserialize_from_file();
        KNOWHERE_CONFIG_DECLARE_FIELD(blockmax_block_size)
            .description("block size for blockmax-based algorithms")
            .set_default(64)
            .set_range(1, 65535, true, true)
            .for_train_and_search()
            .for_deserialize()
            .for_deserialize_from_file();
    }
};  // class SparseInvertedIndexConfig

}  // namespace knowhere

#endif  // SPARSE_INVERTED_INDEX_CONFIG_H
