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

#include "knowhere/index/index.h"

#include "fmt/format.h"
#include "folly/futures/Future.h"
#include "knowhere/comp/thread_pool.h"
#include "knowhere/comp/time_recorder.h"
#include "knowhere/dataset.h"
#include "knowhere/expected.h"
#include "knowhere/log.h"

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
#include "knowhere/prometheus_client.h"
#include "knowhere/tracer.h"
#endif

namespace knowhere {

inline Status
LoadConfig(BaseConfig* cfg, const Json& json, knowhere::PARAM_TYPE param_type, const std::string& method,
           std::string* const msg = nullptr) {
    Json json_(json);
    auto res = Config::FormatAndCheck(*cfg, json_, msg);
    LOG_KNOWHERE_DEBUG_ << method << " config dump: " << json_.dump();
    RETURN_IF_ERROR(res);
    return Config::Load(*cfg, json_, param_type, msg);
}

#ifdef KNOWHERE_WITH_CARDINAL
template <typename T>
inline const std::shared_ptr<Interrupt>
Index<T>::BuildAsync(const DataSetPtr dataset, const Json& json, const std::chrono::seconds timeout) {
    auto pool = ThreadPool::GetGlobalBuildThreadPool();
    auto interrupt = std::make_shared<Interrupt>(timeout);
    interrupt->Set(pool->push([this, dataset, json, interrupt]() {
        auto cfg = this->node->CreateConfig();
        RETURN_IF_ERROR(LoadConfig(cfg.get(), json, knowhere::TRAIN, "Build"));

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
        TimeRecorder rc("BuildAsync index ", 2);
        auto res = this->node->BuildAsync(dataset, std::move(cfg), interrupt.get());
        auto time = rc.ElapseFromBegin("done");
        time *= 0.000001;  // convert to s
        knowhere_build_latency.Observe(time);
#else
        auto res = this->node->BuildAsync(dataset, std::move(cfg), Interrupt.get());
#endif
        return res;
    }));
    return interrupt;
}
#else
template <typename T>
inline const std::shared_ptr<Interrupt>
Index<T>::BuildAsync(const DataSetPtr dataset, const Json& json) {
    auto pool = ThreadPool::GetGlobalBuildThreadPool();
    auto interrupt = std::make_shared<Interrupt>();
    interrupt->Set(pool->push([this, dataset, json]() { return this->Build(dataset, json); }));
    return interrupt;
}
#endif

template <typename T>
inline Status
Index<T>::Build(const DataSetPtr dataset, const Json& json) {
    auto cfg = this->node->CreateConfig();
    RETURN_IF_ERROR(LoadConfig(cfg.get(), json, knowhere::TRAIN, "Build"));

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
    TimeRecorder rc("Build index", 2);
    auto res = this->node->Build(dataset, std::move(cfg));
    auto time = rc.ElapseFromBegin("done");
    time *= 0.000001;  // convert to s
    knowhere_build_latency.Observe(time);
#else
    auto res = this->node->Build(dataset, std::move(cfg));
#endif
    return res;
}

template <typename T>
inline Status
Index<T>::Train(const DataSetPtr dataset, const Json& json) {
    auto cfg = this->node->CreateConfig();
    std::string msg;
    RETURN_IF_ERROR(LoadConfig(cfg.get(), json, knowhere::TRAIN, "Train", &msg));
    return this->node->Train(dataset, std::move(cfg));
}

template <typename T>
inline Status
Index<T>::Add(const DataSetPtr dataset, const Json& json) {
    auto cfg = this->node->CreateConfig();
    std::string msg;
    RETURN_IF_ERROR(LoadConfig(cfg.get(), json, knowhere::TRAIN, "Add", &msg));
    return this->node->Add(dataset, std::move(cfg));
}

template <typename T>
inline expected<DataSetPtr>
Index<T>::Search(const DataSetPtr dataset, const Json& json, const BitsetView& bitset_) const {
    auto cfg = this->node->CreateConfig();
    std::string msg;
    const Status load_status = LoadConfig(cfg.get(), json, knowhere::SEARCH, "Search", &msg);
    if (load_status != Status::success) {
        return expected<DataSetPtr>::Err(load_status, msg);
    }
    // when index is immutable, bitset size should always equal to data count in index
    // when index is mutable, it could happen that data count larger than bitset size, see
    // https://github.com/zilliztech/knowhere/issues/70
    // so something must be wrong at caller side when passed bitset size larger than data count
    if (bitset_.size() > (size_t)this->Count()) {
        msg = fmt::format("bitset size should be <= data count, but we get bitset size: {}, data count: {}",
                          bitset_.size(), this->Count());
        LOG_KNOWHERE_ERROR_ << msg;
        return expected<DataSetPtr>::Err(Status::invalid_args, msg);
    }

    const auto bitset = BitsetView(bitset_.data(), bitset_.size(), bitset_.get_filtered_out_num_());

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
    const BaseConfig& b_cfg = static_cast<const BaseConfig&>(*cfg);
    // LCOV_EXCL_START
    std::shared_ptr<tracer::trace::Span> span = nullptr;
    if (b_cfg.trace_id.has_value()) {
        auto trace_id_str = tracer::GetIDFromHexStr(b_cfg.trace_id.value());
        auto span_id_str = tracer::GetIDFromHexStr(b_cfg.span_id.value());
        auto ctx = tracer::TraceContext{(uint8_t*)trace_id_str.c_str(), (uint8_t*)span_id_str.c_str(),
                                        (uint8_t)b_cfg.trace_flags.value()};
        span = tracer::StartSpan("knowhere search", &ctx);
        span->SetAttribute(meta::METRIC_TYPE, b_cfg.metric_type.value());
        span->SetAttribute(meta::TOPK, b_cfg.k.value());
        span->SetAttribute(meta::ROWS, Count());
        span->SetAttribute(meta::DIM, Dim());
        span->SetAttribute(meta::NQ, dataset->GetRows());
    }
    // LCOV_EXCL_STOP

    TimeRecorder rc("Search");
    bool has_trace_id = b_cfg.trace_id.has_value();
    auto k = cfg->k.value();
    auto res = this->node->Search(dataset, std::move(cfg), bitset);
    auto time = rc.ElapseFromBegin("done");
    time *= 0.001;  // convert to ms
    knowhere_search_latency.Observe(time);
    knowhere_search_topk.Observe(k);

    // LCOV_EXCL_START
    if (has_trace_id) {
        span->End();
    }
    // LCOV_EXCL_STOP
#else
    auto res = this->node->Search(dataset, std::move(cfg), bitset);
#endif
    return res;
}

template <typename T>
inline expected<std::vector<std::shared_ptr<IndexNode::iterator>>>
Index<T>::AnnIterator(const DataSetPtr dataset, const Json& json, const BitsetView& bitset_,
                      bool use_knowhere_search_pool) const {
    auto cfg = this->node->CreateConfig();
    std::string msg;
    Status status = LoadConfig(cfg.get(), json, knowhere::ITERATOR, "Iterator", &msg);
    if (status != Status::success) {
        return expected<std::vector<std::shared_ptr<IndexNode::iterator>>>::Err(status, msg);
    }
    // when index is immutable, bitset size should always equal to data count in index
    // when index is mutable, it could happen that data count larger than bitset size, see
    // https://github.com/zilliztech/knowhere/issues/70
    // so something must be wrong at caller side when passed bitset size larger than data count
    if (bitset_.size() > (size_t)this->Count()) {
        msg = fmt::format("bitset size should be <= data count, but we get bitset size: {}, data count: {}",
                          bitset_.size(), this->Count());
        LOG_KNOWHERE_ERROR_ << msg;
        return expected<std::vector<std::shared_ptr<IndexNode::iterator>>>::Err(Status::invalid_args, msg);
    }

    const auto bitset = BitsetView(bitset_.data(), bitset_.size(), bitset_.get_filtered_out_num_());

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
    // note that this time includes only the initial search phase of iterator.
    TimeRecorder rc("AnnIterator");
    auto res = this->node->AnnIterator(dataset, std::move(cfg), bitset, use_knowhere_search_pool);
    auto time = rc.ElapseFromBegin("done");
    time *= 0.001;  // convert to ms
    knowhere_search_latency.Observe(time);
#else
    auto res = this->node->AnnIterator(dataset, std::move(cfg), bitset, use_knowhere_search_pool);
#endif
    return res;
}

template <typename T>
inline expected<DataSetPtr>
Index<T>::RangeSearch(const DataSetPtr dataset, const Json& json, const BitsetView& bitset_) const {
    auto cfg = this->node->CreateConfig();
    std::string msg;
    auto status = LoadConfig(cfg.get(), json, knowhere::RANGE_SEARCH, "RangeSearch", &msg);
    if (status != Status::success) {
        return expected<DataSetPtr>::Err(status, std::move(msg));
    }
    // when index is immutable, bitset size should always equal to data count in index
    // when index is mutable, it could happen that data count larger than bitset size, see
    // https://github.com/zilliztech/knowhere/issues/70
    // so something must be wrong at caller side when passed bitset size larger than data count
    if (bitset_.size() > (size_t)this->Count()) {
        msg = fmt::format("bitset size should be <= data count, but we get bitset size: {}, data count: {}",
                          bitset_.size(), this->Count());
        LOG_KNOWHERE_ERROR_ << msg;
        return expected<DataSetPtr>::Err(Status::invalid_args, msg);
    }

    const auto bitset = BitsetView(bitset_.data(), bitset_.size(), bitset_.get_filtered_out_num_());

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
    const BaseConfig& b_cfg = static_cast<const BaseConfig&>(*cfg);
    // LCOV_EXCL_START
    std::shared_ptr<tracer::trace::Span> span = nullptr;
    if (b_cfg.trace_id.has_value()) {
        auto trace_id_str = tracer::GetIDFromHexStr(b_cfg.trace_id.value());
        auto span_id_str = tracer::GetIDFromHexStr(b_cfg.span_id.value());
        auto ctx = tracer::TraceContext{(uint8_t*)trace_id_str.c_str(), (uint8_t*)span_id_str.c_str(),
                                        (uint8_t)b_cfg.trace_flags.value()};
        span = tracer::StartSpan("knowhere range search", &ctx);
        span->SetAttribute(meta::METRIC_TYPE, b_cfg.metric_type.value());
        span->SetAttribute(meta::RADIUS, b_cfg.radius.value());
        if (b_cfg.range_filter.value() != defaultRangeFilter) {
            span->SetAttribute(meta::RANGE_FILTER, b_cfg.range_filter.value());
        }
        span->SetAttribute(meta::ROWS, Count());
        span->SetAttribute(meta::DIM, Dim());
        span->SetAttribute(meta::NQ, dataset->GetRows());
    }
    // LCOV_EXCL_STOP

    TimeRecorder rc("Range Search");
    bool has_trace_id = b_cfg.trace_id.has_value();
    auto res = this->node->RangeSearch(dataset, std::move(cfg), bitset);
    auto time = rc.ElapseFromBegin("done");
    time *= 0.001;  // convert to ms
    knowhere_range_search_latency.Observe(time);

    // LCOV_EXCL_START
    if (has_trace_id) {
        span->End();
    }
    // LCOV_EXCL_STOP
#else
    auto res = this->node->RangeSearch(dataset, std::move(cfg), bitset);
#endif
    return res;
}

template <typename T>
inline expected<DataSetPtr>
Index<T>::GetVectorByIds(const DataSetPtr dataset) const {
    return this->node->GetVectorByIds(dataset);
}

template <typename T>
inline bool
Index<T>::HasRawData(const std::string& metric_type) const {
    return this->node->HasRawData(metric_type);
}

template <typename T>
inline bool
Index<T>::IsAdditionalScalarSupported(bool is_mv_only) const {
    return this->node->IsAdditionalScalarSupported(is_mv_only);
}

template <typename T>
inline expected<DataSetPtr>
Index<T>::GetIndexMeta(const Json& json) const {
    auto cfg = this->node->CreateConfig();
    std::string msg;
    auto status = LoadConfig(cfg.get(), json, knowhere::FEDER, "GetIndexMeta", &msg);
    if (status != Status::success) {
        return expected<DataSetPtr>::Err(status, msg);
    }
    return this->node->GetIndexMeta(std::move(cfg));
}

template <typename T>
inline Status
Index<T>::Serialize(BinarySet& binset) const {
    return this->node->Serialize(binset);
}

template <typename T>
inline Status
Index<T>::Deserialize(BinarySet&& binset, const Json& json) {
    Json json_(json);
    auto cfg = this->node->CreateConfig();
    {
        auto res = Config::FormatAndCheck(*cfg, json_);
        LOG_KNOWHERE_DEBUG_ << "Deserialize config dump: " << json_.dump();
        if (res != Status::success) {
            return res;
        }
    }
    auto res = Config::Load(*cfg, json_, knowhere::DESERIALIZE);
    if (res != Status::success) {
        return res;
    }

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
    TimeRecorder rc("Load index", 2);
    res = this->node->Deserialize(std::move(binset), std::move(cfg));
    auto time = rc.ElapseFromBegin("done");
    time *= 0.001;  // convert to ms
    knowhere_load_latency.Observe(time);
#else
    res = this->node->Deserialize(std::move(binset), std::move(cfg));
#endif
    return res;
}

template <typename T>
inline Status
Index<T>::DeserializeFromFile(const std::string& filename, const Json& json) {
    Json json_(json);
    auto cfg = this->node->CreateConfig();
    {
        auto res = Config::FormatAndCheck(*cfg, json_);
        LOG_KNOWHERE_DEBUG_ << "DeserializeFromFile config dump: " << json_.dump();
        if (res != Status::success) {
            return res;
        }
    }
    auto res = Config::Load(*cfg, json_, knowhere::DESERIALIZE_FROM_FILE);
    if (res != Status::success) {
        return res;
    }

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
    TimeRecorder rc("Load index from file", 2);
    res = this->node->DeserializeFromFile(filename, std::move(cfg));
    auto time = rc.ElapseFromBegin("done");
    time *= 0.001;  // convert to ms
    knowhere_load_latency.Observe(time);
#else
    res = this->node->DeserializeFromFile(filename, std::move(cfg));
#endif
    return res;
}

template <typename T>
inline int64_t
Index<T>::Dim() const {
    return this->node->Dim();
}

template <typename T>
inline int64_t
Index<T>::Size() const {
    return this->node->Size();
}

template <typename T>
inline int64_t
Index<T>::Count() const {
    return this->node->Count();
}

template <typename T>
inline std::string
Index<T>::Type() const {
    return this->node->Type();
}

template class Index<IndexNode>;

}  // namespace knowhere
