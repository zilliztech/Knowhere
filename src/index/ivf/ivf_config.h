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

#ifndef IVF_CONFIG_H
#define IVF_CONFIG_H

#include "knowhere/config.h"
#include "simd/hook.h"

namespace knowhere {
class IvfConfig : public BaseConfig {
 public:
    CFG_INT nlist;
    CFG_INT nprobe;
    CFG_BOOL use_elkan;
    CFG_BOOL ensure_topk_full;  // only take affect on temp index(IVF_FLAT_CC) now
    CFG_INT max_empty_result_buckets;
    KNOHWERE_DECLARE_CONFIG(IvfConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(nlist)
            .description("number of inverted lists.")
            .set_default(128)
            .for_train()
            .set_range(1, 65536);
        KNOWHERE_CONFIG_DECLARE_FIELD(nprobe)
            .set_default(8)
            .description("number of probes at query time.")
            .for_search()
            .for_iterator()
            .set_range(1, 65536);
        KNOWHERE_CONFIG_DECLARE_FIELD(use_elkan)
            .set_default(true)
            .description("whether to use elkan algorithm")
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(ensure_topk_full)
            .set_default(true)
            .description("whether to make sure topk results full")
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(max_empty_result_buckets)
            .set_default(2)
            .description("the maximum of continuous buckets with empty result")
            .for_range_search()
            .set_range(1, 65536);
    }
};

class IvfFlatConfig : public IvfConfig {};

class IvfFlatCcConfig : public IvfFlatConfig {
 public:
    CFG_INT ssize;
    KNOHWERE_DECLARE_CONFIG(IvfFlatCcConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(ssize)
            .description("segment size")
            .set_default(48)
            .for_train()
            .set_range(32, 2048);
    }
};

class IvfPqConfig : public IvfConfig {
 public:
    CFG_INT m;
    CFG_INT nbits;
    KNOHWERE_DECLARE_CONFIG(IvfPqConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(m).description("m").for_train().set_range(1, 65536);
        // FAISS rejects nbits > 24, because it is not practical
        KNOWHERE_CONFIG_DECLARE_FIELD(nbits).description("nbits").set_default(8).for_train().set_range(1, 24);
    }

    Status
    CheckAndAdjust(PARAM_TYPE param_type, std::string* err_msg) override {
        switch (param_type) {
            case PARAM_TYPE::TRAIN: {
                if (dim.has_value() && m.has_value()) {
                    int vec_dim = dim.value();
                    int param_m = m.value();
                    if (vec_dim % param_m != 0) {
                        std::string msg =
                            "The dimension of a vector (dim) should be a multiple of the number of subquantizers "
                            "(m). Dimension: " +
                            std::to_string(vec_dim) + ", m: " + std::to_string(param_m);
                        return HandleError(err_msg, msg, Status::invalid_args);
                    }
                }
            }
            default:
                break;
        }
        return Status::success;
    }
};

class ScannConfig : public IvfFlatConfig {
 public:
    CFG_INT reorder_k;
    CFG_BOOL with_raw_data;
    CFG_INT sub_dim;
    KNOHWERE_DECLARE_CONFIG(ScannConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(reorder_k)
            .description("reorder k used for refining")
            .allow_empty_without_default()
            .set_range(1, std::numeric_limits<CFG_INT::value_type>::max())
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(with_raw_data)
            .description("with raw data in index")
            .set_default(true)
            .for_static()
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(sub_dim)
            .description("sub dim of each sub dimension space")
            .set_default(2)
            .for_train()
            .set_range(1, 65536);
    }

    Status
    CheckAndAdjust(PARAM_TYPE param_type, std::string* err_msg) override {
        switch (param_type) {
            case PARAM_TYPE::TRAIN: {
                // TODO: handle vec_dim % vec_sub_dim != 0 with scann
                if (dim.has_value()) {
                    int vec_dim = dim.value();
                    int vec_sub_dim = sub_dim.value();
                    if (vec_dim % vec_sub_dim != 0) {
                        std::string msg =
                            "The dimension of a vector (dim) should be a multiple of sub_dim. Dimension:" +
                            std::to_string(vec_dim) + ", sub_dim:" + std::to_string(vec_sub_dim);
                        return HandleError(err_msg, msg, Status::invalid_args);
                    }
                }
            }
            case PARAM_TYPE::SEARCH: {
                if (!faiss::support_pq_fast_scan) {
                    LOG_KNOWHERE_ERROR_ << "SCANN index is not supported on the current CPU model, avx2 support is "
                                           "needed for x86 arch.";
                    return Status::invalid_instruction_set;
                }
                if (!reorder_k.has_value()) {
                    reorder_k = k.value();
                } else if (reorder_k.value() < k.value()) {
                    if (!err_msg) {
                        err_msg = new std::string();
                    }
                    std::string msg = "reorder_k(" + std::to_string(reorder_k.value()) + ") should be larger than k(" +
                                      std::to_string(k.value()) + ")";
                    return HandleError(err_msg, msg, Status::out_of_range_in_json);
                }
                break;
            }
            default: {
                if (!faiss::support_pq_fast_scan) {
                    std::string msg =
                        "SCANN index is not supported on the current CPU model, avx2 support is "
                        "needed for x86 arch.";
                    return HandleError(err_msg, msg, Status::invalid_instruction_set);
                }
                break;
            }
        }
        return Status::success;
    }
};

class IvfSqConfig : public IvfConfig {};

class IvfBinConfig : public IvfConfig {
    Status
    CheckAndAdjust(PARAM_TYPE param_type, std::string* err_msg) override {
        if (param_type == PARAM_TYPE::TRAIN) {
            constexpr std::array<std::string_view, 2> legal_metric_list{"HAMMING", "JACCARD"};
            std::string metric = metric_type.value();
            if (std::find(legal_metric_list.begin(), legal_metric_list.end(), metric) == legal_metric_list.end()) {
                std::string msg = "metric type " + metric + " not found or not supported, supported: [HAMMING JACCARD]";
                return HandleError(err_msg, msg, Status::invalid_metric_type);
            }
        }
        return Status::success;
    }
};

class IvfSqCcConfig : public IvfFlatCcConfig {
 public:
    // user can use code size to control ivf_sq_cc quntizer type
    CFG_INT code_size;
    // IVF_SQ_CC holds all vectors in file when raw_data_store_prefix has value;
    // cc index is a just-in-time index, raw data is avaliable after training if raw_data_store_prefix has value.
    // ivf sq cc index will not keep raw data after using binaryset to create a new ivf sq cc index.
    CFG_STRING raw_data_store_prefix;
    KNOHWERE_DECLARE_CONFIG(IvfSqCcConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(code_size)
            .set_default(8)
            .description("code size, range in [4, 6, 8 and 16]")
            .for_train();
        KNOWHERE_CONFIG_DECLARE_FIELD(raw_data_store_prefix)
            .description("Raw data will be set in this prefix path")
            .for_train()
            .for_static()
            .allow_empty_without_default();
    };
    Status
    CheckAndAdjust(PARAM_TYPE param_type, std::string* err_msg) override {
        if (param_type == PARAM_TYPE::TRAIN) {
            auto code_size_v = code_size.value();
            auto legal_code_size_list = std::vector<int>{4, 6, 8, 16};
            if (std::find(legal_code_size_list.begin(), legal_code_size_list.end(), code_size_v) ==
                legal_code_size_list.end()) {
                std::string msg =
                    "compress a vector into (code_size * dim)/8 bytes, code size value should be in 4, 6, 8 and 16";
                return HandleError(err_msg, msg, Status::invalid_value_in_json);
            }
        }
        return Status::success;
    }
};

}  // namespace knowhere

#endif /* IVF_CONFIG_H */
