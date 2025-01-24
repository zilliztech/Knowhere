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

#pragma once

#include <string>

namespace knowhere {

using IndexType = std::string;
using IndexVersion = int32_t;

namespace IndexEnum {

constexpr const char* INVALID = "";

constexpr const char* INDEX_FAISS_BIN_IDMAP = "BIN_FLAT";
constexpr const char* INDEX_FAISS_BIN_IVFFLAT = "BIN_IVF_FLAT";

constexpr const char* INDEX_FAISS_IDMAP = "FLAT";
constexpr const char* INDEX_FAISS_IVFFLAT = "IVF_FLAT";
constexpr const char* INDEX_FAISS_IVFFLAT_CC = "IVF_FLAT_CC";
constexpr const char* INDEX_FAISS_IVFPQ = "IVF_PQ";
constexpr const char* INDEX_FAISS_SCANN = "SCANN";
constexpr const char* INDEX_FAISS_IVFSQ8 = "IVF_SQ8";
constexpr const char* INDEX_FAISS_IVFSQ_CC = "IVF_SQ_CC";

constexpr const char* INDEX_FAISS_GPU_IDMAP = "GPU_FAISS_FLAT";
constexpr const char* INDEX_FAISS_GPU_IVFFLAT = "GPU_FAISS_IVF_FLAT";
constexpr const char* INDEX_FAISS_GPU_IVFPQ = "GPU_FAISS_IVF_PQ";
constexpr const char* INDEX_FAISS_GPU_IVFSQ8 = "GPU_FAISS_IVF_SQ8";

constexpr const char* INDEX_RAFT_BRUTEFORCE = "GPU_RAFT_BRUTE_FORCE";
constexpr const char* INDEX_RAFT_IVFFLAT = "GPU_RAFT_IVF_FLAT";
constexpr const char* INDEX_RAFT_IVFPQ = "GPU_RAFT_IVF_PQ";
constexpr const char* INDEX_RAFT_CAGRA = "GPU_RAFT_CAGRA";

constexpr const char* INDEX_GPU_BRUTEFORCE = "GPU_BRUTE_FORCE";
constexpr const char* INDEX_GPU_IVFFLAT = "GPU_IVF_FLAT";
constexpr const char* INDEX_GPU_IVFPQ = "GPU_IVF_PQ";
constexpr const char* INDEX_GPU_CAGRA = "GPU_CAGRA";

constexpr const char* INDEX_HNSW = "HNSW";
constexpr const char* INDEX_HNSW_SQ = "HNSW_SQ";
constexpr const char* INDEX_HNSW_PQ = "HNSW_PQ";
constexpr const char* INDEX_HNSW_PRQ = "HNSW_PRQ";

constexpr const char* INDEX_DISKANN = "DISKANN";

constexpr const char* INDEX_SPARSE_INVERTED_INDEX = "SPARSE_INVERTED_INDEX";
constexpr const char* INDEX_SPARSE_WAND = "SPARSE_WAND";
constexpr const char* INDEX_SPARSE_INVERTED_INDEX_CC = "SPARSE_INVERTED_INDEX_CC";
constexpr const char* INDEX_SPARSE_WAND_CC = "SPARSE_WAND_CC";
}  // namespace IndexEnum

namespace ClusterEnum {
constexpr const char* CLUSTER_KMEANS = "KMEANS";
}  // namespace ClusterEnum

namespace meta {
constexpr const char* INDEX_TYPE = "index_type";
constexpr const char* METRIC_TYPE = "metric_type";
constexpr const char* DATA_PATH = "data_path";
constexpr const char* INDEX_PREFIX = "index_prefix";
constexpr const char* INDEX_ENGINE_VERSION = "index_engine_version";
constexpr const char* RETRIEVE_FRIENDLY = "retrieve_friendly";
constexpr const char* DIM = "dim";
constexpr const char* TENSOR = "tensor";
constexpr const char* ROWS = "rows";
constexpr const char* NQ = "nq";
constexpr const char* IDS = "ids";
constexpr const char* DISTANCE = "distance";
constexpr const char* LIMS = "lims";
constexpr const char* TOPK = "k";
constexpr const char* RANGE_SEARCH_K = "range_search_k";
constexpr const char* RETAIN_ITERATOR_ORDER = "retain_iterator_order";
constexpr const char* RADIUS = "radius";
constexpr const char* RANGE_FILTER = "range_filter";
constexpr const char* INPUT_IDS = "input_ids";
constexpr const char* INPUT_BEG_ID = "input_begin_id";
constexpr const char* OUTPUT_TENSOR = "output_tensor";
constexpr const char* DEVICE_ID = "gpu_id";
constexpr const char* NUM_BUILD_THREAD = "num_build_thread";
constexpr const char* TRACE_VISIT = "trace_visit";
constexpr const char* JSON_INFO = "json_info";
constexpr const char* JSON_ID_SET = "json_id_set";
constexpr const char* TRACE_ID = "trace_id";
constexpr const char* SPAN_ID = "span_id";
constexpr const char* TRACE_FLAGS = "trace_flags";
constexpr const char* SCALAR_INFO = "scalar_info";
constexpr const char* MATERIALIZED_VIEW_SEARCH_INFO = "materialized_view_search_info";
constexpr const char* MATERIALIZED_VIEW_OPT_FIELDS_PATH = "opt_fields_path";
constexpr const char* MAX_EMPTY_RESULT_BUCKETS = "max_empty_result_buckets";
constexpr const char* BM25_K1 = "bm25_k1";
constexpr const char* BM25_B = "bm25_b";
// average document length
constexpr const char* BM25_AVGDL = "bm25_avgdl";
constexpr const char* DIM_MAX_SCORE_RATIO = "dim_max_score_ratio";
};  // namespace meta

namespace indexparam {
// IVF Params
constexpr const char* NPROBE = "nprobe";
constexpr const char* NLIST = "nlist";
constexpr const char* USE_ELKAN = "use_elkan";
constexpr const char* NBITS = "nbits";  // PQ/SQ
constexpr const char* M = "m";          // PQ param for IVFPQ
constexpr const char* SSIZE = "ssize";
constexpr const char* REORDER_K = "reorder_k";
constexpr const char* WITH_RAW_DATA = "with_raw_data";
constexpr const char* ENSURE_TOPK_FULL = "ensure_topk_full";
constexpr const char* CODE_SIZE = "code_size";
constexpr const char* RAW_DATA_STORE_PREFIX = "raw_data_store_prefix";

// RAFT Params
constexpr const char* REFINE_RATIO = "refine_ratio";
constexpr const char* CACHE_DATASET_ON_DEVICE = "cache_dataset_on_device";

// RAFT-specific IVF Params
constexpr const char* KMEANS_N_ITERS = "kmeans_n_iters";
constexpr const char* KMEANS_TRAINSET_FRACTION = "kmeans_trainset_fraction";
constexpr const char* ADAPTIVE_CENTERS = "adaptive_centers";                              // IVF FLAT
constexpr const char* CODEBOOK_KIND = "codebook_kind";                                    // IVF PQ
constexpr const char* FORCE_RANDOM_ROTATION = "force_random_rotation";                    // IVF PQ
constexpr const char* CONSERVATIVE_MEMORY_ALLOCATION = "conservative_memory_allocation";  // IVF PQ
constexpr const char* LUT_DTYPE = "lut_dtype";                                            // IVF PQ
constexpr const char* INTERNAL_DISTANCE_DTYPE = "internal_distance_dtype";                // IVF PQ
constexpr const char* PREFERRED_SHMEM_CARVEOUT = "preferred_shmem_carveout";              // IVF PQ

// CAGRA Params
constexpr const char* INTERMEDIATE_GRAPH_DEGREE = "intermediate_graph_degree";
constexpr const char* GRAPH_DEGREE = "graph_degree";
constexpr const char* ITOPK_SIZE = "itopk_size";
constexpr const char* MAX_QUERIES = "max_queries";
constexpr const char* BUILD_ALGO = "build_algo";
constexpr const char* SEARCH_ALGO = "search_algo";
constexpr const char* TEAM_SIZE = "team_size";
constexpr const char* SEARCH_WIDTH = "search_width";
constexpr const char* MIN_ITERATIONS = "min_iterations";
constexpr const char* MAX_ITERATIONS = "max_iterations";
constexpr const char* THREAD_BLOCK_SIZE = "thread_block_size";
constexpr const char* HASHMAP_MODE = "hashmap_mode";
constexpr const char* HASHMAP_MIN_BITLEN = "hashmap_min_bitlen";
constexpr const char* HASHMAP_MAX_FILL_RATE = "hashmap_max_fill_rate";
constexpr const char* NN_DESCENT_NITER = "nn_descent_niter";
constexpr const char* ADAPT_FOR_CPU = "adapt_for_cpu";

// HNSW Params
constexpr const char* EFCONSTRUCTION = "efConstruction";
constexpr const char* HNSW_M = "M";
constexpr const char* EF = "ef";
constexpr const char* OVERVIEW_LEVELS = "overview_levels";

// DISKANN Params
constexpr const char* MAX_DEGREE = "max_degree";
constexpr const char* PQ_CODE_BUDGET_GB = "pq_code_budget_gb";
constexpr const char* BUILD_DRAM_BUDGET_GB = "build_dram_budget_gb";
constexpr const char* BEAMWIDTH = "beamwidth";
constexpr const char* SEARCH_CACHE_BUDGET_GB = "search_cache_budget_gb";
constexpr const char* SEARCH_LIST_SIZE = "search_list_size";

// FAISS additional Params
constexpr const char* HNSW_REFINE = "refine";
constexpr const char* HNSW_REFINE_K = "refine_k";
constexpr const char* HNSW_REFINE_TYPE = "refine_type";
constexpr const char* SQ_TYPE = "sq_type";  // for IVF_SQ and HNSW_SQ
constexpr const char* PRQ_NUM = "nrq";      // for PRQ, number of redisual quantizers

// Sparse Inverted Index Params
constexpr const char* INVERTED_INDEX_ALGO = "inverted_index_algo";
constexpr const char* DROP_RATIO_BUILD = "drop_ratio_build";
constexpr const char* DROP_RATIO_SEARCH = "drop_ratio_search";
constexpr const char* BLOCKMAX_BLOCK_SIZE = "blockmax_block_size";
}  // namespace indexparam

using MetricType = std::string;

namespace metric {
constexpr const char* IP = "IP";
constexpr const char* L2 = "L2";
constexpr const char* COSINE = "COSINE";
constexpr const char* HAMMING = "HAMMING";
constexpr const char* JACCARD = "JACCARD";
constexpr const char* SUBSTRUCTURE = "SUBSTRUCTURE";
constexpr const char* SUPERSTRUCTURE = "SUPERSTRUCTURE";
constexpr const char* BM25 = "BM25";
}  // namespace metric

enum VecType {
    VECTOR_BINARY = 100,
    VECTOR_FLOAT = 101,
    VECTOR_FLOAT16 = 102,
    VECTOR_BFLOAT16 = 103,
    VECTOR_SPARSE_FLOAT = 104,
    VECTOR_INT8 = 105,
};  // keep the same value as milvus proto define

}  // namespace knowhere
