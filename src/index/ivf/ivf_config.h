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
    CFG_BOOL ensure_topk_full;  // only take affect on temp index(IVF_FLAT_CC) now
    KNOHWERE_DECLARE_CONFIG(IvfConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(nlist)
            .set_default(128)
            .description("number of inverted lists.")
            .for_train()
            .set_range(1, 65536);
        KNOWHERE_CONFIG_DECLARE_FIELD(nprobe)
            .set_default(8)
            .description("number of probes at query time.")
            .for_search()
            .set_range(1, 65536);
        KNOWHERE_CONFIG_DECLARE_FIELD(ensure_topk_full)
            .set_default(true)
            .description("whether to make sure topk results full")
            .for_search();
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
        KNOWHERE_CONFIG_DECLARE_FIELD(m).description("m").set_default(32).for_train().set_range(1, 65536);
        KNOWHERE_CONFIG_DECLARE_FIELD(nbits).description("nbits").set_default(8).for_train().set_range(1, 64);
    }
};

class ScannConfig : public IvfFlatConfig {
 public:
    CFG_INT reorder_k;
    CFG_BOOL with_raw_data;
    KNOHWERE_DECLARE_CONFIG(ScannConfig) {
        KNOWHERE_CONFIG_DECLARE_FIELD(reorder_k)
            .description("reorder k used for refining")
            .allow_empty_without_default()
            .set_range(1, std::numeric_limits<CFG_INT::value_type>::max())
            .for_search();
        KNOWHERE_CONFIG_DECLARE_FIELD(with_raw_data)
            .description("with raw data in index")
            .set_default(true)
            .for_train();
    }

    inline Status
    CheckAndAdjustForSearch(std::string* err_msg) override {
        if (!faiss::support_pq_fast_scan) {
            *err_msg = "SCANN index is not supported on the current CPU model, avx2 support is needed for x86 arch.";
            LOG_KNOWHERE_ERROR_ << *err_msg;
            return Status::invalid_instruction_set;
        }
        if (!reorder_k.has_value()) {
            reorder_k = k.value();
        } else if (reorder_k.value() < k.value()) {
            *err_msg = "reorder_k(" + std::to_string(reorder_k.value()) + ") should be larger than k(" +
                       std::to_string(k.value()) + ")";
            LOG_KNOWHERE_ERROR_ << *err_msg;
            return Status::out_of_range_in_json;
        }

        return Status::success;
    }

    inline Status
    CheckAndAdjustForRangeSearch(std::string* err_msg) override {
        if (!faiss::support_pq_fast_scan) {
            *err_msg = "SCANN index is not supported on the current CPU model, avx2 support is needed for x86 arch.";
            LOG_KNOWHERE_ERROR_ << *err_msg;
            return Status::invalid_instruction_set;
        }
        return Status::success;
    }

    inline Status
    CheckAndAdjustForBuild() override {
        if (!faiss::support_pq_fast_scan) {
            LOG_KNOWHERE_ERROR_
                << "SCANN index is not supported on the current CPU model, avx2 support is needed for x86 arch.";
            return Status::invalid_instruction_set;
        }
        return Status::success;
    }
};

class IvfSqConfig : public IvfConfig {};

class IvfBinConfig : public IvfConfig {};

}  // namespace knowhere

#endif /* IVF_CONFIG_H */
