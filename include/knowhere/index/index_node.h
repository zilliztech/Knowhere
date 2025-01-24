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

#ifndef INDEX_NODE_H
#define INDEX_NODE_H

#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "knowhere/binaryset.h"
#include "knowhere/bitsetview.h"
#include "knowhere/config.h"
#include "knowhere/dataset.h"
#include "knowhere/expected.h"
#include "knowhere/object.h"
#include "knowhere/operands.h"
#include "knowhere/range_util.h"
#include "knowhere/utils.h"
#include "knowhere/version.h"

#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
#include "knowhere/comp/thread_pool.h"
#endif

namespace knowhere {

class Interrupt;

class IndexNode : public Object {
 public:
    IndexNode(const int32_t ver) : version_(ver) {
    }

    IndexNode() : version_(Version::GetDefaultVersion()) {
    }

    IndexNode(const IndexNode& other) : version_(other.version_) {
    }

    IndexNode(const IndexNode&& other) : version_(other.version_) {
    }

    /**
     * @brief Builds the index using the provided dataset and configuration.
     *
     * Mostly, this method combines the `Train` and `Add` steps to create the index structure, but it can be overridden
     * if the index doesn't support Train-Add pattern, such as immutable indexes like DiskANN.
     *
     * @param dataset Dataset to build the index from.
     * @param cfg
     * @return Status.
     *
     * @note Indexes need to be ready to search after `Build` is called. TODO:@liliu-z DiskANN is an exception and need
     * to be revisited.
     * @note Providing support for the async interface is possible.Since the config object needs to be held in a future
     * or lambda function, a smart pointer is required to delay its release.
     * @note Since the build interface aggregates calls to both the train and add interfaces, `unique_ptr` cannot be
     * shared, so `shared_ptr` is used instead.
     */
    virtual Status
    Build(const DataSetPtr dataset, std::shared_ptr<Config> cfg) {
        RETURN_IF_ERROR(Train(dataset, cfg));
        return Add(dataset, std::move(cfg));
    }

/*
 *@ @brief Builds the index using the provided dataset,configuration and handle.
 */
#ifdef KNOWHERE_WITH_CARDINAL
    virtual Status
    BuildAsync(const DataSetPtr dataset, std::shared_ptr<Config> cfg, const Interrupt* = nullptr) {
        return Build(dataset, std::move(cfg));
    }
#endif

    /**
     * @brief Trains the index model using the provided dataset and configuration.
     *
     * @param dataset Dataset used to train the index.
     * @param cfg
     * @return Status.
     *
     * @note This interface is only available for growable indexes. For immutable indexes like DiskANN, this method
     * should return an error.
     * @note Providing support for the async interface is possible.Since the config object needs to be held in a future
     * or lambda function, a smart pointer is required to delay its release.
     * @note Since the build interface aggregates calls to both the train and add interfaces, `unique_ptr` cannot be
     * shared, so `shared_ptr` is used instead.
     */
    virtual Status
    Train(const DataSetPtr dataset, std::shared_ptr<Config> cfg) = 0;

    /**
     * @brief Adds data to the trained index.
     *
     * @param dataset Dataset to add to the index.
     * @param cfg
     * @return Status
     *
     * @note
     * 1. This interface is only available for growable indexes. For immutable indexes like DiskANN, this method
     * should return an error.
     * 2. This method need to be thread safe when called with search methods like @see Search, @see RangeSearch and @see
     * AnnIterator.
     * @note Providing support for the async interface is possible.Since the config object needs to be held in a future
     * or lambda function, a smart pointer is required to delay its release.
     * @note Since the build interface aggregates calls to both the train and add interfaces, `unique_ptr` cannot be
     * shared, so `shared_ptr` is used instead.
     */
    virtual Status
    Add(const DataSetPtr dataset, std::shared_ptr<Config> cfg) = 0;

    /**
     * @brief Performs a search operation on the index.
     *
     * @param dataset Query vectors.
     * @param cfg
     * @param bitset A BitsetView object for filtering results.
     * @return An expected<> object containing the search results or an error.
     * @note Since the config object needs to be held in a future or lambda function, a smart pointer is required to
     * delay its release.
     */
    virtual expected<DataSetPtr>
    Search(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const = 0;

    // not thread safe.
    class iterator {
     public:
        virtual std::pair<int64_t, float>
        Next() = 0;
        [[nodiscard]] virtual bool
        HasNext() = 0;
        virtual ~iterator() {
        }
    };
    using IteratorPtr = std::shared_ptr<iterator>;

    virtual expected<std::vector<IteratorPtr>>
    AnnIterator(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset,
                bool use_knowhere_search_pool = true) const {
        return expected<std::vector<std::shared_ptr<iterator>>>::Err(
            Status::not_implemented, "annIterator not supported for current index type");
    }

    /**
     * @brief Performs a range search operation on the index.
     *
     * This method provides a default implementation of range search based on the `AnnIterator`, assuming the iterator
     * will buffer an expanded range and return the closest elements on each Next() call. It can be overridden by
     * derived classes for more efficient implementations.
     *
     * @param dataset Query vectors.
     * @param cfg
     * @param bitset A BitsetView object for filtering results.
     * @return An expected<> object containing the range search results or an error.
     * @note Since the config object needs to be held in a future or lambda function, a smart pointer is required to
     * delay its release.
     */
    virtual expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, std::unique_ptr<Config> cfg,
                const BitsetView& bitset) const {  // TODO: @alwayslove2013 test with mock AnnIterator after we
                                                   // introduced mock framework into knowhere. Currently this is tested
                                                   // in test_sparse.cc with real sparse vector index.
        const auto base_cfg = static_cast<const BaseConfig&>(*cfg);
        const float closer_bound = base_cfg.range_filter.value();
        const bool has_closer_bound = closer_bound != defaultRangeFilter;
        float further_bound = base_cfg.radius.value();

        const bool the_larger_the_closer = IsMetricType(base_cfg.metric_type.value(), metric::IP) ||
                                           IsMetricType(base_cfg.metric_type.value(), metric::COSINE) ||
                                           IsMetricType(base_cfg.metric_type.value(), metric::BM25);
        auto is_first_closer = [&the_larger_the_closer](const float dist_1, const float dist_2) {
            return the_larger_the_closer ? dist_1 > dist_2 : dist_1 < dist_2;
        };
        auto too_close = [&is_first_closer, &closer_bound](float dist) { return is_first_closer(dist, closer_bound); };
        auto same_or_too_far = [&is_first_closer, &further_bound](float dist) {
            return !is_first_closer(dist, further_bound);
        };

        /** The `range_search_k` is used to early terminate the iterator-search.
         * - `range_search_k < 0` means no early termination.
         * - `range_search_k == 0` will return empty results.
         * - Note that the number of results is not guaranteed to be exactly range_search_k, it may be more or less.
         * */
        const int32_t range_search_k = base_cfg.range_search_k.value();
        LOG_KNOWHERE_DEBUG_ << "range_search_k: " << range_search_k;
        if (range_search_k == 0) {
            auto nq = dataset->GetRows();
            std::vector<std::vector<int64_t>> result_id_array(nq);
            std::vector<std::vector<float>> result_dist_array(nq);
            auto range_search_result = GetRangeSearchResult(result_dist_array, result_id_array, the_larger_the_closer,
                                                            nq, further_bound, closer_bound);
            return GenResultDataSet(nq, std::move(range_search_result));
        }

        // The range_search function has utilized the search_pool to concurrently handle various queries.
        // To prevent potential deadlocks, the iterator for a single query no longer requires additional thread
        //   control over the next() call.
        auto its_or = AnnIterator(dataset, std::move(cfg), bitset, false);
        if (!its_or.has_value()) {
            return expected<DataSetPtr>::Err(its_or.error(),
                                             "RangeSearch failed due to AnnIterator failure: " + its_or.what());
        }

        const auto its = its_or.value();
        const auto nq = its.size();
        std::vector<std::vector<int64_t>> result_id_array(nq);
        std::vector<std::vector<float>> result_dist_array(nq);

        const bool retain_iterator_order = base_cfg.retain_iterator_order.value();
        LOG_KNOWHERE_DEBUG_ << "retain_iterator_order: " << retain_iterator_order;

        /**
         * use ordered iterator (retain_iterator_order == true)
         * - terminate iterator if next distance exceeds `further_bound`.
         * - terminate iterator if get enough results. (`range_search_k`)
         * */
        auto task_with_ordered_iterator = [&](size_t idx) {
            auto it = its[idx];
            while (it->HasNext()) {
                auto [id, dist] = it->Next();
                if (has_closer_bound && too_close(dist)) {
                    continue;
                }
                if (same_or_too_far(dist)) {
                    break;
                }
                result_id_array[idx].push_back(id);
                result_dist_array[idx].push_back(dist);
                if (range_search_k >= 0 && static_cast<int32_t>(result_id_array[idx].size()) >= range_search_k) {
                    break;
                }
            }
        };

        /**
         * use default unordered iterator (retain_iterator_order == false)
         * - terminate iterator if next distance [consecutively] exceeds `further_bound` several times.
         * - if get enough results (`range_search_k`), update a `tighter_further_bound`, to early terminate iterator.
         * */
        const auto range_search_level = base_cfg.range_search_level.value();  // from 0 to 0.5
        LOG_KNOWHERE_DEBUG_ << "range_search_level: " << range_search_level;
        auto task_with_unordered_iterator = [&](size_t idx) {
            // max-heap, use top (the current kth-furthest dist) as the further_bound if size == range_search_k
            std::priority_queue<float, std::vector<float>, decltype(is_first_closer)> early_stop_further_bounds(
                is_first_closer);
            auto it = its[idx];
            size_t num_next = 0;
            size_t num_consecutive_over_further_bound = 0;
            float tighter_further_bound = base_cfg.radius.value();
            auto same_or_too_far = [&is_first_closer, &tighter_further_bound](float dist) {
                return !is_first_closer(dist, tighter_further_bound);
            };
            while (it->HasNext()) {
                auto [id, dist] = it->Next();
                num_next++;
                if (has_closer_bound && too_close(dist)) {
                    continue;
                }
                if (same_or_too_far(dist)) {
                    num_consecutive_over_further_bound++;
                    if (num_consecutive_over_further_bound >
                        static_cast<size_t>(std::ceil(num_next * range_search_level))) {
                        break;
                    }
                    continue;
                }
                if (range_search_k > 0) {
                    if (static_cast<int32_t>(early_stop_further_bounds.size()) < range_search_k) {
                        early_stop_further_bounds.emplace(dist);
                    } else {
                        early_stop_further_bounds.pop();
                        early_stop_further_bounds.emplace(dist);
                        tighter_further_bound = early_stop_further_bounds.top();
                    }
                }
                num_consecutive_over_further_bound = 0;
                result_id_array[idx].push_back(id);
                result_dist_array[idx].push_back(dist);
            }
        };
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
        std::vector<folly::Future<folly::Unit>> futs;
        futs.reserve(nq);
        if (retain_iterator_order) {
            for (size_t i = 0; i < nq; i++) {
                futs.emplace_back(ThreadPool::GetGlobalSearchThreadPool()->push([&, idx = i]() {
                    ThreadPool::ScopedSearchOmpSetter setter(1);
                    task_with_ordered_iterator(idx);
                }));
            }
        } else {
            for (size_t i = 0; i < nq; i++) {
                futs.emplace_back(ThreadPool::GetGlobalSearchThreadPool()->push([&, idx = i]() {
                    ThreadPool::ScopedSearchOmpSetter setter(1);
                    task_with_unordered_iterator(idx);
                }));
            }
        }
        WaitAllSuccess(futs);
#else
        if (retain_iterator_order) {
            for (size_t i = 0; i < nq; i++) {
                task_with_ordered_iterator(i);
            }
        } else {
            for (size_t i = 0; i < nq; i++) {
                task_with_unordered_iterator(i);
            }
        }
#endif

        auto range_search_result = GetRangeSearchResult(result_dist_array, result_id_array, the_larger_the_closer, nq,
                                                        further_bound, closer_bound);
        return GenResultDataSet(nq, std::move(range_search_result));
    }

    /**
     * @brief Retrieves raw vectors by their IDs from the index.
     *
     * @param dataset Dataset containing the IDs of the vectors to retrieve.
     * @return An expected<> object containing the retrieved vectors or an error.
     *
     * @note
     * 1. This method may return an error if the index does not contain raw data. The returned raw data must be exactly
     * the same as the input data when we do @see Add or @see Build. For example, if the datatype is BF16, then we need
     * to return a dataset with BF16 vectors.
     * 2. It doesn't guarantee the index contains raw data, so it's better to check with @see HasRawData() before
     */
    virtual expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const = 0;

    /**
     * @brief Checks if the index contains raw vector data.
     *
     * @param metric_type The metric type used in the index.
     * @return true if the index contains raw data, false otherwise.
     */
    virtual bool
    HasRawData(const std::string& metric_type) const = 0;

    virtual bool
    IsAdditionalScalarSupported(bool is_mv_only) const {
        return false;
    }

    /**
     * @unused Milvus is not using this method, so it cannot guarantee all indexes implement this method.
     *
     * This is for Feder, and we can ignore it for now.
     */
    virtual expected<DataSetPtr>
    GetIndexMeta(std::unique_ptr<Config> cfg) const = 0;

    /**
     * @brief Serializes the index to a binary set.
     *
     * @param binset The BinarySet to store the serialized index.
     * @return Status indicating success or failure of the serialization.
     */
    virtual Status
    Serialize(BinarySet& binset) const = 0;

    /**
     * @brief Deserializes the index from a binary set.
     *
     * @param binset The BinarySet containing the serialized index.
     * @param config
     * @return Status indicating success or failure of the deserialization.
     *
     * @note
     * 1. The index should be ready to search after deserialization.
     * 2. For immutable indexes, the path for now if Build->Serialize->Deserialize->Search.
     * 3. Since the config object needs to be held in a future or lambda function, a smart pointer is required to delay
     * its release.
     */
    virtual Status
    Deserialize(BinarySet&& binset, std::shared_ptr<Config> config) = 0;

    /**
     * @brief Deserializes the index from a file.
     *
     * This method is mostly used for mmap deserialization. However, it has some conflicts with the FileManager that we
     * used to deserialize DiskANN. TODO: @liliu-z some redesign is needed here.
     *
     * @param filename Path to the file containing the serialized index.
     * @param config
     * @return Status indicating success or failure of the deserialization.
     */
    virtual Status
    DeserializeFromFile(const std::string& filename, std::shared_ptr<Config> config) = 0;

    virtual std::unique_ptr<BaseConfig>
    CreateConfig() const = 0;

    /**
     * @brief Gets the dimensionality of the vectors in the index.
     *
     * @return The number of dimensions as an int64_t.
     */
    virtual int64_t
    Dim() const = 0;

    /**
     * @unused Milvus is not using this method, so it cannot guarantee all indexes implement this method.
     *
     * @brief Gets the memory usage of the index in bytes.
     *
     * @return The size of the index as an int64_t.
     * @note This method doesn't have to be very accurate.
     */
    virtual int64_t
    Size() const = 0;

    /**
     * @brief Gets the number of vectors in the index.
     *
     * @return The count of vectors as an int64_t.
     */
    virtual int64_t
    Count() const = 0;

    virtual std::string
    Type() const = 0;

    virtual ~IndexNode() {
    }

 protected:
    BinarySet binarySet_;
    Version version_;
};

// Common superclass for iterators that expand search range as needed. Subclasses need
//   to override `next_batch` which will add expanded vectors to the results. For indexes
//   with quantization, override `raw_distance`.
// Internally, this structure uses the same priority queue class, but may multiply all
//   incoming distances to (-1) value in order to turn max priority queue into a min one.
// If use_knowhere_search_pool is True (the default), the iterator->Next() will be scheduled by the
//   knowhere_search_thread_pool.
//   If False, will Not involve thread scheduling internally, so please take caution.
class IndexIterator : public IndexNode::iterator {
 public:
    IndexIterator(bool larger_is_closer, bool use_knowhere_search_pool = true, float refine_ratio = 0.0f,
                  bool retain_iterator_order = false)
        : refine_ratio_(refine_ratio),
          refine_(refine_ratio != 0.0f),
          retain_iterator_order_(retain_iterator_order),
          use_knowhere_search_pool_(use_knowhere_search_pool),
          sign_(larger_is_closer ? -1 : 1) {
    }

    std::pair<int64_t, float>
    Next() override {
        if (!initialized_) {
            initialize();
        }
        auto& q = refined_res_.empty() ? res_ : refined_res_;
        if (q.empty()) {
            throw std::runtime_error("No more elements");
        }
        auto ret = q.top();
        q.pop();

        auto update_next_func = [&]() {
            UpdateNext();
            if (retain_iterator_order_) {
                while (HasNext()) {
                    auto& q = refined_res_.empty() ? res_ : refined_res_;
                    auto next_ret = q.top();
                    // with the help of `sign_`, both `res_` and `refine_res` are min-heap.
                    //   such as `COSINE`, `-dist` will be inserted to `res_` or `refine_res`.
                    // just make sure that the next value is greater than or equal to the current value.
                    if (next_ret.val >= ret.val) {
                        break;
                    }
                    q.pop();
                    UpdateNext();
                }
            }
        };
        if (use_knowhere_search_pool_) {
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
            std::vector<folly::Future<folly::Unit>> futs;
            futs.emplace_back(ThreadPool::GetGlobalSearchThreadPool()->push([&]() {
                ThreadPool::ScopedSearchOmpSetter setter(1);
                update_next_func();
            }));
            WaitAllSuccess(futs);
#else
            update_next_func();
#endif
        } else {
            update_next_func();
        }

        return std::make_pair(ret.id, ret.val * sign_);
    }

    [[nodiscard]] bool
    HasNext() override {
        if (!initialized_) {
            initialize();
        }
        return !res_.empty() || !refined_res_.empty();
    }

    virtual void
    initialize() {
        if (initialized_) {
            throw std::runtime_error("initialize should not be called twice");
        }
        UpdateNext();
        initialized_ = true;
    }

 protected:
    virtual void
    next_batch(std::function<void(const std::vector<DistId>&)> batch_handler) = 0;
    // will be called only if refine_ratio_ is not 0.
    virtual float
    raw_distance(int64_t) {
        if (!refine_) {
            throw std::runtime_error("raw_distance should not be called for indexes without quantization");
        }
        throw std::runtime_error("raw_distance not implemented");
    }

    const float refine_ratio_;
    const bool refine_;

    std::priority_queue<DistId, std::vector<DistId>, std::greater<DistId>> res_;
    // unused if refine_ is false
    std::priority_queue<DistId, std::vector<DistId>, std::greater<DistId>> refined_res_;

 private:
    inline size_t
    min_refine_size() const {
        // TODO: maybe make this configurable
        return std::max((size_t)20, (size_t)(res_.size() * refine_ratio_));
    }

    void
    UpdateNext() {
        auto batch_handler = [this](const std::vector<DistId>& batch) {
            if (batch.empty()) {
                return;
            }
            for (const auto& dist_id : batch) {
                res_.emplace(dist_id.id, dist_id.val * sign_);
            }
            if (refine_) {
                while (!res_.empty() && (refined_res_.empty() || refined_res_.size() < min_refine_size())) {
                    auto pair = res_.top();
                    res_.pop();
                    refined_res_.emplace(pair.id, raw_distance(pair.id) * sign_);
                }
            }
        };
        next_batch(batch_handler);
    }

    bool initialized_ = false;
    bool retain_iterator_order_ = false;
    bool use_knowhere_search_pool_ = true;
    const int64_t sign_;
};

// An iterator implementation that accepts a function to get distances and ids list and returns them in order.
// We do not directly accept a distance list as input. The main reason for this is to minimize the initialization time
//   for all types of iterators in the `ANNIterator` interface, moving heavy computations to the first '->Next()' call.
//   This way, the iterator initialization does not need to perform any concurrent acceleration, and the search pool
//   only needs to handle the heavy work of `->Next()`
class PrecomputedDistanceIterator : public IndexNode::iterator {
 public:
    PrecomputedDistanceIterator(std::function<std::vector<DistId>()> compute_dist_func, bool larger_is_closer,
                                bool use_knowhere_search_pool = true)
        : compute_dist_func_(compute_dist_func),
          larger_is_closer_(larger_is_closer),
          use_knowhere_search_pool_(use_knowhere_search_pool) {
    }

    std::pair<int64_t, float>
    Next() override {
        if (!initialized_) {
            initialize();
        }
        if (use_knowhere_search_pool_) {
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
            std::vector<folly::Future<folly::Unit>> futs;
            futs.emplace_back(ThreadPool::GetGlobalSearchThreadPool()->push([&]() {
                ThreadPool::ScopedSearchOmpSetter setter(1);
                sort_next();
            }));
            WaitAllSuccess(futs);
#else
            sort_next();
#endif
        } else {
            sort_next();
        }
        auto& result = results_[next_++];
        return std::make_pair(result.id, result.val);
    }

    [[nodiscard]] bool
    HasNext() override {
        if (!initialized_) {
            initialize();
        }
        return next_ < results_.size() && results_[next_].id != -1;
    }

    void
    initialize() {
        if (initialized_) {
            throw std::runtime_error("initialize should not be called twice");
        }
        if (use_knowhere_search_pool_) {
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
            std::vector<folly::Future<folly::Unit>> futs;
            futs.emplace_back(ThreadPool::GetGlobalSearchThreadPool()->push([&]() {
                ThreadPool::ScopedSearchOmpSetter setter(1);
                results_ = compute_dist_func_();
            }));
            WaitAllSuccess(futs);
#else
            results_ = compute_dist_func_();
#endif
        } else {
            results_ = compute_dist_func_();
        }
        sort_size_ = get_sort_size(results_.size());
        sort_next();
        initialized_ = true;
    }

 private:
    static inline size_t
    get_sort_size(size_t rows) {
        return std::max((size_t)50000, rows / 10);
    }

    // sort the next sort_size_ elements
    inline void
    sort_next() {
        if (next_ < sorted_) {
            return;
        }
        size_t current_end = std::min(results_.size(), sorted_ + sort_size_);
        if (larger_is_closer_) {
            std::partial_sort(results_.begin() + sorted_, results_.begin() + current_end, results_.end(),
                              std::greater<DistId>());
        } else {
            std::partial_sort(results_.begin() + sorted_, results_.begin() + current_end, results_.end(),
                              std::less<DistId>());
        }

        sorted_ = current_end;
    }

    std::function<std::vector<DistId>()> compute_dist_func_;
    const bool larger_is_closer_;
    bool use_knowhere_search_pool_ = true;
    bool initialized_ = false;
    std::vector<DistId> results_;
    size_t next_ = 0;
    size_t sorted_ = 0;
    size_t sort_size_ = 0;
};

}  // namespace knowhere

#endif /* INDEX_NODE_H */
