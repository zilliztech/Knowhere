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

#ifndef SPARSE_INVERTED_INDEX_H
#define SPARSE_INVERTED_INDEX_H

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <vector>

#include "index/sparse/sparse_inverted_index_config.h"
#include "io/memory_io.h"
#include "knowhere/bitsetview.h"
#include "knowhere/expected.h"
#include "knowhere/log.h"
#include "knowhere/sparse_utils.h"
#include "knowhere/utils.h"

namespace knowhere::sparse {
template <typename T>
class BaseInvertedIndex {
 public:
    virtual ~BaseInvertedIndex() = default;

    virtual Status
    Save(MemoryIOWriter& writer) = 0;

    // supplement_target_filename: when in mmap mode, we need an extra file to store the mmaped index data structure.
    // this file will be created during loading and deleted in the destructor.
    virtual Status
    Load(MemoryIOReader& reader, int map_flags = MAP_SHARED, const std::string& supplement_target_filename = "") = 0;

    virtual Status
    Train(const SparseRow<T>* data, size_t rows, float drop_ratio_build) = 0;

    virtual Status
    Add(const SparseRow<T>* data, size_t rows, int64_t dim) = 0;

    virtual void
    Search(const SparseRow<T>& query, size_t k, float drop_ratio_search, float* distances, label_t* labels,
           size_t refine_factor, const BitsetView& bitset, const DocValueComputer<T>& computer) const = 0;

    virtual std::vector<float>
    GetAllDistances(const SparseRow<T>& query, float drop_ratio_search, const BitsetView& bitset,
                    const DocValueComputer<T>& computer) const = 0;

    virtual float
    GetRawDistance(const label_t id, const SparseRow<T>& query, const DocValueComputer<T>& computer) const = 0;

    virtual void
    GetVectorById(const label_t id, SparseRow<T>& output) const = 0;

    virtual expected<DocValueComputer<T>>
    GetDocValueComputer(const SparseInvertedIndexConfig& cfg) const = 0;

    virtual bool
    IsApproximated() const = 0;

    [[nodiscard]] virtual size_t
    size() const = 0;

    [[nodiscard]] virtual size_t
    n_rows() const = 0;

    [[nodiscard]] virtual size_t
    n_cols() const = 0;
};

template <typename T, bool use_wand = false, bool bm25 = false, bool mmapped = false>
class InvertedIndex : public BaseInvertedIndex<T> {
 public:
    explicit InvertedIndex() {
    }

    ~InvertedIndex() override {
        if constexpr (mmapped) {
            if (map_ != nullptr) {
                auto res = munmap(map_, map_byte_size_);
                if (res != 0) {
                    LOG_KNOWHERE_ERROR_ << "Failed to munmap when deleting sparse InvertedIndex: " << strerror(errno);
                }
                map_ = nullptr;
                map_byte_size_ = 0;
            }
            if (map_fd_ != -1) {
                // closing the file descriptor will also cause the file to be deleted.
                close(map_fd_);
                map_fd_ = -1;
            }
        }
    }

    void
    SetBM25Params(float k1, float b, float avgdl, float max_score_ratio) {
        bm25_params_ = std::make_unique<BM25Params>(k1, b, avgdl, max_score_ratio);
    }

    expected<DocValueComputer<T>>
    GetDocValueComputer(const SparseInvertedIndexConfig& cfg) const override {
        // if metric_type is set in config, it must match with how the index was built.
        auto metric_type = cfg.metric_type;
        if constexpr (!bm25) {
            if (metric_type.has_value() && !IsMetricType(metric_type.value(), metric::IP)) {
                auto msg =
                    "metric type not match, expected: " + std::string(metric::IP) + ", got: " + metric_type.value();
                return expected<DocValueComputer<T>>::Err(Status::invalid_metric_type, msg);
            }
            return GetDocValueOriginalComputer<T>();
        }
        if (metric_type.has_value() && !IsMetricType(metric_type.value(), metric::BM25)) {
            auto msg =
                "metric type not match, expected: " + std::string(metric::BM25) + ", got: " + metric_type.value();
            return expected<DocValueComputer<T>>::Err(Status::invalid_metric_type, msg);
        }
        // avgdl must be supplied during search
        if (!cfg.bm25_avgdl.has_value()) {
            return expected<DocValueComputer<T>>::Err(Status::invalid_args, "avgdl must be supplied during searching");
        }
        auto avgdl = cfg.bm25_avgdl.value();
        if constexpr (use_wand) {
            // wand: search time k1/b must equal load time config.
            if ((cfg.bm25_k1.has_value() && cfg.bm25_k1.value() != bm25_params_->k1) ||
                ((cfg.bm25_b.has_value() && cfg.bm25_b.value() != bm25_params_->b))) {
                return expected<DocValueComputer<T>>::Err(
                    Status::invalid_args, "search time k1/b must equal load time config for WAND index.");
            }
            return GetDocValueBM25Computer<T>(bm25_params_->k1, bm25_params_->b, avgdl);
        } else {
            // inverted index: search time k1/b may override load time config.
            auto k1 = cfg.bm25_k1.has_value() ? cfg.bm25_k1.value() : bm25_params_->k1;
            auto b = cfg.bm25_b.has_value() ? cfg.bm25_b.value() : bm25_params_->b;
            return GetDocValueBM25Computer<T>(k1, b, avgdl);
        }
    }

    Status
    Save(MemoryIOWriter& writer) override {
        /**
         * Layout:
         *
         * 1. size_t rows
         * 2. size_t cols
         * 3. T value_threshold_
         * 4. for each row:
         *     1. size_t len
         *     2. for each non-zero value:
         *        1. table_t idx
         *        2. T val
         *
         * inverted_lut_ and max_score_in_dim_ not serialized, they will be
         * constructed dynamically during deserialization.
         *
         * Data are densely packed in serialized bytes and no padding is added.
         */
        writeBinaryPOD(writer, n_rows_internal());
        writeBinaryPOD(writer, n_cols_internal());
        writeBinaryPOD(writer, value_threshold_);
        for (size_t i = 0; i < n_rows_internal(); ++i) {
            auto& row = raw_data_[i];
            writeBinaryPOD(writer, row.size());
            if (row.size() == 0) {
                continue;
            }
            writer.write(row.data(), row.size() * SparseRow<T>::element_size());
        }
        return Status::success;
    }

    Status
    Load(MemoryIOReader& reader, int map_flags = MAP_SHARED,
         const std::string& supplement_target_filename = "") override {
        int64_t rows;
        readBinaryPOD(reader, rows);
        // previous versions used the signness of rows to indicate whether to
        // use wand. now we use a template parameter to control this thus simply
        // take the absolute value of rows.
        rows = std::abs(rows);
        readBinaryPOD(reader, max_dim_);
        readBinaryPOD(reader, value_threshold_);
        if (value_threshold_ > 0) {
            drop_during_build_ = true;
        }

        if constexpr (mmapped) {
            RETURN_IF_ERROR(PrepareMmap(reader, rows, map_flags, supplement_target_filename));
        } else {
            raw_data_.reserve(rows);
            if constexpr (bm25) {
                bm25_params_->row_sums.reserve(rows);
            }
        }

        for (int64_t i = 0; i < rows; ++i) {
            size_t count;
            readBinaryPOD(reader, count);
            if constexpr (mmapped) {
                raw_data_.emplace_back(count, reader.data() + reader.tellg(), false);
                reader.advance(count * SparseRow<T>::element_size());
            } else {
                raw_data_.emplace_back(count);
                if (count > 0) {
                    reader.read(raw_data_[i].data(), count * SparseRow<T>::element_size());
                }
            }
            add_row_to_index(raw_data_[i], i);
        }
        return Status::success;
    }

    // memory in reader must be guaranteed to be valid during the lifetime of this object.
    Status
    PrepareMmap(MemoryIOReader& reader, size_t rows, int map_flags, const std::string& supplement_target_filename) {
        const auto initial_reader_location = reader.tellg();
        const auto nnz = (reader.remaining() - (rows * sizeof(size_t))) / SparseRow<T>::element_size();

        // count raw vector idx occurrences
        std::unordered_map<table_t, size_t> idx_counts;
        for (size_t i = 0; i < rows; ++i) {
            size_t row_nnz;
            readBinaryPOD(reader, row_nnz);
            if (row_nnz == 0) {
                continue;
            }
            for (size_t j = 0; j < row_nnz; ++j) {
                table_t idx;
                readBinaryPOD(reader, idx);
                idx_counts[idx]++;
                // skip value
                reader.advance(sizeof(T));
            }
        }
        // reset reader to the beginning
        reader.seekg(initial_reader_location);

        auto raw_data_byte_size = rows * sizeof(typename decltype(raw_data_)::value_type);
        auto inverted_lut_byte_size = idx_counts.size() * sizeof(typename decltype(inverted_lut_)::value_type);
        // actually due to drop_ratio_build, the number of non-zero values that will be added to the luts is
        // less than nnz. but since the memory is mmapped, it is ok to still allocate some extra space for those
        // dropped values.
        auto luts_byte_size = nnz * sizeof(typename decltype(inverted_lut_)::value_type::value_type);
        auto max_score_in_dim_byte_size = idx_counts.size() * sizeof(typename decltype(max_score_in_dim_)::value_type);
        size_t row_sums_byte_size = 0;

        map_byte_size_ = raw_data_byte_size + inverted_lut_byte_size + luts_byte_size;
        if constexpr (use_wand) {
            map_byte_size_ += max_score_in_dim_byte_size;
        }
        if constexpr (bm25) {
            row_sums_byte_size = rows * sizeof(typename decltype(bm25_params_->row_sums)::value_type);
            map_byte_size_ += row_sums_byte_size;
        }

        std::ofstream temp_file(supplement_target_filename, std::ios::binary | std::ios::trunc);
        if (!temp_file) {
            LOG_KNOWHERE_ERROR_ << "Failed to create mmap file when loading sparse InvertedIndex: " << strerror(errno);
            return Status::disk_file_error;
        }
        temp_file.close();

        std::filesystem::resize_file(supplement_target_filename, map_byte_size_);

        map_fd_ = open(supplement_target_filename.c_str(), O_RDWR);
        if (map_fd_ == -1) {
            LOG_KNOWHERE_ERROR_ << "Failed to open mmap file when loading sparse InvertedIndex: " << strerror(errno);
            return Status::disk_file_error;
        }
        // file will disappear in the filesystem immediately but the actual file will not be deleted
        // until the file descriptor is closed in the destructor.
        std::filesystem::remove(supplement_target_filename);

        // clear MAP_PRIVATE flag: we need to write to this mmapped memory/file,
        // MAP_PRIVATE triggers copy-on-write and uses extra anonymous memory.
        map_flags &= ~MAP_PRIVATE;
        map_flags |= MAP_SHARED;

        map_ = static_cast<char*>(mmap(nullptr, map_byte_size_, PROT_READ | PROT_WRITE, map_flags, map_fd_, 0));
        if (map_ == MAP_FAILED) {
            LOG_KNOWHERE_ERROR_ << "Failed to create mmap when loading sparse InvertedIndex: " << strerror(errno)
                                << ", size: " << map_byte_size_ << " on file: " << supplement_target_filename;
            return Status::disk_file_error;
        }
        if (madvise(map_, map_byte_size_, MADV_RANDOM) != 0) {
            LOG_KNOWHERE_WARNING_ << "Failed to madvise mmap when loading sparse InvertedIndex: " << strerror(errno);
        }

        char* ptr = map_;

        // initialize containers memory.
        raw_data_.initialize(ptr, raw_data_byte_size);
        ptr += raw_data_byte_size;
        inverted_lut_.initialize(ptr, inverted_lut_byte_size);
        ptr += inverted_lut_byte_size;

        if constexpr (use_wand) {
            max_score_in_dim_.initialize(ptr, max_score_in_dim_byte_size);
            ptr += max_score_in_dim_byte_size;
        }

        if constexpr (bm25) {
            bm25_params_->row_sums.initialize(ptr, row_sums_byte_size);
            ptr += row_sums_byte_size;
        }

        size_t dim_id = 0;
        for (const auto& [idx, count] : idx_counts) {
            dim_map_[idx] = dim_id;
            auto& lut = inverted_lut_.emplace_back();
            auto lut_byte_size = count * sizeof(typename decltype(inverted_lut_)::value_type::value_type);
            lut.initialize(ptr, lut_byte_size);
            ptr += lut_byte_size;
            if constexpr (use_wand) {
                max_score_in_dim_.emplace_back(0);
            }
            ++dim_id;
        }
        // in mmap mode, next_dim_id_ should never be used, but still assigning for consistency.
        next_dim_id_ = dim_id;

        return Status::success;
    }

    // Non zero drop ratio is only supported for static index, i.e. data should
    // include all rows that'll be added to the index.
    Status
    Train(const SparseRow<T>* data, size_t rows, float drop_ratio_build) override {
        if constexpr (mmapped) {
            throw std::invalid_argument("mmapped InvertedIndex does not support Train");
        } else {
            if (drop_ratio_build == 0.0f) {
                return Status::success;
            }
            // TODO: maybe i += 10 to down sample to speed up.
            size_t amount = 0;
            for (size_t i = 0; i < rows; ++i) {
                amount += data[i].size();
            }
            if (amount == 0) {
                return Status::success;
            }
            std::vector<T> vals;
            vals.reserve(amount);
            for (size_t i = 0; i < rows; ++i) {
                for (size_t j = 0; j < data[i].size(); ++j) {
                    vals.push_back(fabs(data[i][j].val));
                }
            }
            value_threshold_ = get_threshold(vals, drop_ratio_build);
            drop_during_build_ = true;
            return Status::success;
        }
    }

    Status
    Add(const SparseRow<T>* data, size_t rows, int64_t dim) override {
        if constexpr (mmapped) {
            throw std::invalid_argument("mmapped InvertedIndex does not support Add");
        } else {
            auto current_rows = n_rows_internal();
            if (current_rows > 0 && drop_during_build_) {
                LOG_KNOWHERE_ERROR_ << "Not allowed to add data to a built index with drop_ratio_build > 0.";
                return Status::invalid_args;
            }
            if ((size_t)dim > max_dim_) {
                max_dim_ = dim;
            }

            raw_data_.insert(raw_data_.end(), data, data + rows);
            if constexpr (bm25) {
                bm25_params_->row_sums.reserve(current_rows + rows);
            }
            for (size_t i = 0; i < rows; ++i) {
                add_row_to_index(data[i], current_rows + i);
            }
            return Status::success;
        }
    }

    void
    Search(const SparseRow<T>& query, size_t k, float drop_ratio_search, float* distances, label_t* labels,
           size_t refine_factor, const BitsetView& bitset, const DocValueComputer<T>& computer) const override {
        // initially set result distances to NaN and labels to -1
        std::fill(distances, distances + k, std::numeric_limits<float>::quiet_NaN());
        std::fill(labels, labels + k, -1);
        if (query.size() == 0) {
            return;
        }

        std::vector<T> values(query.size());
        for (size_t i = 0; i < query.size(); ++i) {
            values[i] = std::abs(query[i].val);
        }
        auto q_threshold = get_threshold(values, drop_ratio_search);

        // if no data was dropped during both build and search, no refinement is
        // needed.
        if (!drop_during_build_ && drop_ratio_search == 0) {
            refine_factor = 1;
        }
        MaxMinHeap<T> heap(k * refine_factor);
        if constexpr (!use_wand) {
            search_brute_force(query, q_threshold, heap, bitset, computer);
        } else {
            search_wand(query, q_threshold, heap, bitset, computer);
        }

        if (refine_factor == 1) {
            collect_result(heap, distances, labels);
        } else {
            refine_and_collect(query, heap, k, distances, labels, computer);
        }
    }

    // Returned distances are inaccurate based on the drop_ratio.
    std::vector<float>
    GetAllDistances(const SparseRow<T>& query, float drop_ratio_search, const BitsetView& bitset,
                    const DocValueComputer<T>& computer) const override {
        if (query.size() == 0) {
            return {};
        }
        std::vector<T> values(query.size());
        for (size_t i = 0; i < query.size(); ++i) {
            values[i] = std::abs(query[i].val);
        }
        auto q_threshold = get_threshold(values, drop_ratio_search);
        auto distances = compute_all_distances(query, q_threshold, computer);
        for (size_t i = 0; i < distances.size(); ++i) {
            if (bitset.empty() || !bitset.test(i)) {
                continue;
            }
            distances[i] = 0.0f;
        }
        return distances;
    }

    float
    GetRawDistance(const label_t id, const SparseRow<T>& query, const DocValueComputer<T>& computer) const override {
        T doc_sum = bm25 ? bm25_params_->row_sums.at(id) : 0;
        return query.dot(raw_data_[id], computer, doc_sum);
    }

    void
    GetVectorById(const label_t id, SparseRow<T>& output) const override {
        output = raw_data_[id];
    }

    [[nodiscard]] size_t
    size() const override {
        size_t res = sizeof(*this);
        for (size_t i = 0; i < raw_data_.size(); ++i) {
            res += raw_data_[i].memory_usage();
        }
        res += dim_map_.size() *
               (sizeof(typename decltype(dim_map_)::key_type) + sizeof(typename decltype(dim_map_)::mapped_type));

        if constexpr (mmapped) {
            return res + map_byte_size_;
        } else {
            res += sizeof(typename decltype(raw_data_)::value_type) * raw_data_.capacity();

            res += sizeof(typename decltype(inverted_lut_)::value_type) * inverted_lut_.capacity();
            for (size_t i = 0; i < inverted_lut_.size(); ++i) {
                res += sizeof(typename decltype(inverted_lut_)::value_type::value_type) * inverted_lut_[i].capacity();
            }
            if constexpr (use_wand) {
                res += sizeof(typename decltype(max_score_in_dim_)::value_type) * max_score_in_dim_.capacity();
            }
            return res;
        }
    }

    [[nodiscard]] size_t
    n_rows() const override {
        return n_rows_internal();
    }

    [[nodiscard]] size_t
    n_cols() const override {
        return n_cols_internal();
    }

    [[nodiscard]] virtual bool
    IsApproximated() const override {
        return drop_during_build_;
    }

 private:
    // Given a vector of values, returns the threshold value.
    // All values strictly smaller than the threshold will be ignored.
    // values will be modified in this function.
    inline T
    get_threshold(std::vector<T>& values, float drop_ratio) const {
        // drop_ratio is in [0, 1) thus drop_count is guaranteed to be less
        // than values.size().
        auto drop_count = static_cast<size_t>(drop_ratio * values.size());
        if (drop_count == 0) {
            return 0;
        }
        auto pos = values.begin() + drop_count;
        std::nth_element(values.begin(), pos, values.end());
        return *pos;
    }

    size_t
    n_rows_internal() const {
        return raw_data_.size();
    }

    size_t
    n_cols_internal() const {
        return max_dim_;
    }

    std::vector<float>
    compute_all_distances(const SparseRow<T>& q_vec, T q_threshold, const DocValueComputer<T>& computer) const {
        std::vector<float> scores(n_rows_internal(), 0.0f);
        for (size_t idx = 0; idx < q_vec.size(); ++idx) {
            auto [i, v] = q_vec[idx];
            if (v < q_threshold || i >= n_cols_internal()) {
                continue;
            }
            auto dim_id = dim_map_.find(i);
            if (dim_id == dim_map_.end()) {
                continue;
            }
            auto& lut = inverted_lut_[dim_id->second];
            // TODO: improve with SIMD
            for (size_t j = 0; j < lut.size(); j++) {
                auto [doc_id, val] = lut[j];
                T val_sum = bm25 ? bm25_params_->row_sums.at(doc_id) : 0;
                scores[doc_id] += v * computer(val, val_sum);
            }
        }
        return scores;
    }

    // find the top-k candidates using brute force search, k as specified by the capacity of the heap.
    // any value in q_vec that is smaller than q_threshold and any value with dimension >= n_cols() will be ignored.
    // TODO: may switch to row-wise brute force if filter rate is high. Benchmark needed.
    void
    search_brute_force(const SparseRow<T>& q_vec, T q_threshold, MaxMinHeap<T>& heap, const BitsetView& bitset,
                       const DocValueComputer<T>& computer) const {
        auto scores = compute_all_distances(q_vec, q_threshold, computer);
        for (size_t i = 0; i < n_rows_internal(); ++i) {
            if ((bitset.empty() || !bitset.test(i)) && scores[i] != 0) {
                heap.push(i, scores[i]);
            }
        }
    }

    // LUT supports size() and operator[] which returns an SparseIdVal.
    template <typename LUT>
    struct Cursor {
     public:
        Cursor(const LUT& lut, size_t num_vec, float max_score, float q_value, const BitsetView bitset)
            : lut_(lut),
              lut_size_(lut.size()),
              total_num_vec_(num_vec),
              max_score_(max_score),
              q_value_(q_value),
              bitset_(bitset) {
            while (loc_ < lut_size_ && !bitset_.empty() && bitset_.test(lut_[loc_].id)) {
                loc_++;
            }
            update_cur_vec_id();
        }
        Cursor(const Cursor& rhs) = delete;

        void
        next() {
            next_internal();
            update_cur_vec_id();
        }

        // advance loc until cur_vec_id_ >= vec_id
        void
        seek(table_t vec_id) {
            while (loc_ < lut_size_ && lut_[loc_].id < vec_id) {
                next_internal();
            }
            update_cur_vec_id();
        }

        T
        cur_vec_val() const {
            return lut_[loc_].val;
        }

        const LUT& lut_;
        const size_t lut_size_;
        size_t loc_ = 0;
        size_t total_num_vec_ = 0;
        float max_score_ = 0.0f;
        float q_value_ = 0.0f;
        const BitsetView bitset_;
        table_t cur_vec_id_ = 0;

     private:
        inline void
        update_cur_vec_id() {
            if (loc_ >= lut_size_) {
                cur_vec_id_ = total_num_vec_;
            } else {
                cur_vec_id_ = lut_[loc_].id;
            }
        }

        inline void
        next_internal() {
            loc_++;
            while (loc_ < lut_size_ && !bitset_.empty() && bitset_.test(lut_[loc_].id)) {
                loc_++;
            }
        }
    };  // struct Cursor

    // any value in q_vec that is smaller than q_threshold will be ignored.
    void
    search_wand(const SparseRow<T>& q_vec, T q_threshold, MaxMinHeap<T>& heap, const BitsetView& bitset,
                const DocValueComputer<T>& computer) const {
        auto q_dim = q_vec.size();
        std::vector<std::shared_ptr<Cursor<const typename decltype(inverted_lut_)::value_type&>>> cursors(q_dim);
        size_t valid_q_dim = 0;
        for (size_t i = 0; i < q_dim; ++i) {
            auto [idx, val] = q_vec[i];
            auto dim_id = dim_map_.find(idx);
            if (dim_id == dim_map_.end() || std::abs(val) < q_threshold) {
                continue;
            }
            auto& lut = inverted_lut_[dim_id->second];
            cursors[valid_q_dim++] = std::make_shared<Cursor<decltype(lut)>>(
                lut, n_rows_internal(), max_score_in_dim_[dim_id->second] * val, val, bitset);
        }
        if (valid_q_dim == 0) {
            return;
        }
        cursors.resize(valid_q_dim);
        auto sort_cursors = [&cursors] {
            std::sort(cursors.begin(), cursors.end(), [](auto& x, auto& y) { return x->cur_vec_id_ < y->cur_vec_id_; });
        };
        sort_cursors();
        float upper_bound = 0;
        size_t pivot_cursor_id = 0;
        while (true) {
            float threshold = heap.full() ? heap.top().val : 0;
            bool found_pivot = false;
            for (; pivot_cursor_id < valid_q_dim; ++pivot_cursor_id) {
                if (cursors[pivot_cursor_id]->loc_ >= cursors[pivot_cursor_id]->lut_size_) {
                    break;
                }
                upper_bound += cursors[pivot_cursor_id]->max_score_;
                if (upper_bound > threshold) {
                    found_pivot = true;
                    break;
                }
            }
            if (!found_pivot) {
                break;
            }
            table_t pivot_vec_id = cursors[pivot_cursor_id]->cur_vec_id_;
            if (pivot_vec_id == cursors[0]->cur_vec_id_) {
                float score_sum = 0;
                for (auto& cursor : cursors) {
                    if (cursor->cur_vec_id_ != pivot_vec_id) {
                        break;
                    }
                    T cur_vec_sum = bm25 ? bm25_params_->row_sums.at(cursor->cur_vec_id_) : 0;
                    score_sum += cursor->q_value_ * computer(cursor->cur_vec_val(), cur_vec_sum);
                    cursor->next();
                }
                heap.push(pivot_vec_id, score_sum);
                sort_cursors();
                pivot_cursor_id = 0;
                upper_bound = 0;
            } else {
                size_t next_list = pivot_cursor_id;
                for (; cursors[next_list]->cur_vec_id_ == pivot_vec_id; --next_list) {
                }
                cursors[next_list]->seek(pivot_vec_id);
                if (cursors[next_list]->cur_vec_id_ > pivot_vec_id) {
                    upper_bound -= cursors[next_list]->max_score_;
                    upper_bound -= cursors[pivot_cursor_id]->max_score_;
                    --pivot_cursor_id;
                } else {
                    // the max_score_ of the pivot will be added again in the next loop,
                    // so it needs to be subtracted here.
                    upper_bound -= cursors[pivot_cursor_id]->max_score_;
                }
                for (size_t i = next_list + 1; i < valid_q_dim; ++i) {
                    if (cursors[i]->cur_vec_id_ >= cursors[i - 1]->cur_vec_id_) {
                        break;
                    }
                    std::swap(cursors[i], cursors[i - 1]);
                }
            }
        }
    }

    void
    refine_and_collect(const SparseRow<T>& q_vec, MaxMinHeap<T>& inaccurate, size_t k, float* distances,
                       label_t* labels, const DocValueComputer<T>& computer) const {
        std::priority_queue<SparseIdVal<T>, std::vector<SparseIdVal<T>>, std::greater<SparseIdVal<T>>> heap;

        while (!inaccurate.empty()) {
            auto [u, d] = inaccurate.top();
            inaccurate.pop();

            T u_sum = bm25 ? bm25_params_->row_sums.at(u) : 0;

            auto dist_acc = q_vec.dot(raw_data_[u], computer, u_sum);
            if (heap.size() < k) {
                heap.emplace(u, dist_acc);
            } else if (heap.top().val < dist_acc) {
                heap.pop();
                heap.emplace(u, dist_acc);
            }
        }
        collect_result(heap, distances, labels);
    }

    template <typename HeapType>
    void
    collect_result(HeapType& heap, float* distances, label_t* labels) const {
        int cnt = heap.size();
        for (auto i = cnt - 1; i >= 0; --i) {
            labels[i] = heap.top().id;
            distances[i] = heap.top().val;
            heap.pop();
        }
    }

    inline void
    add_row_to_index(const SparseRow<T>& row, table_t id) {
        [[maybe_unused]] T row_sum = 0;
        for (size_t j = 0; j < row.size(); ++j) {
            auto [idx, val] = row[j];
            if constexpr (bm25) {
                row_sum += val;
            }
            // Skip values equals to or close enough to zero(which contributes
            // little to the total IP score).
            if (val == 0 || (drop_during_build_ && fabs(val) < value_threshold_)) {
                continue;
            }
            auto dim_it = dim_map_.find(idx);
            if (dim_it == dim_map_.end()) {
                if constexpr (mmapped) {
                    throw std::runtime_error("unexpected vector dimension in mmaped InvertedIndex");
                }
                dim_it = dim_map_.insert({idx, next_dim_id_++}).first;
                inverted_lut_.emplace_back();
                if constexpr (use_wand) {
                    max_score_in_dim_.emplace_back(0);
                }
            }
            inverted_lut_[dim_it->second].emplace_back(id, val);
            if constexpr (use_wand) {
                auto score = val;
                if constexpr (bm25) {
                    score = bm25_params_->max_score_ratio * bm25_params_->wand_max_score_computer(val, row_sum);
                }
                max_score_in_dim_[dim_it->second] = std::max(max_score_in_dim_[dim_it->second], score);
            }
        }
        if constexpr (bm25) {
            bm25_params_->row_sums.emplace_back(row_sum);
        }
    }

    // key is raw sparse vector dim/idx, value is the mapped dim/idx id in the index.
    std::unordered_map<table_t, uint32_t> dim_map_;

    template <typename U>
    using Vector = std::conditional_t<mmapped, GrowableVectorView<U>, std::vector<U>>;

    // reserve, [], size, emplace_back
    Vector<SparseRow<T>> raw_data_;

    Vector<Vector<SparseIdVal<T>>> inverted_lut_;
    // If we want to drop small values during build, we must first train the
    // index with all the data to compute value_threshold_.
    bool drop_during_build_ = false;
    // when drop_during_build_ is true, any value smaller than value_threshold_
    // will not be added to inverted_lut_. value_threshold_ is set to the
    // drop_ratio_build-th percentile of all absolute values in the index.
    T value_threshold_ = 0.0f;
    Vector<T> max_score_in_dim_;
    size_t max_dim_ = 0;
    uint32_t next_dim_id_ = 0;

    char* map_ = nullptr;
    size_t map_byte_size_ = 0;
    int map_fd_ = -1;

    struct BM25Params {
        float k1;
        float b;
        // row_sums is used to cache the sum of values of each row, which
        // corresponds to the document length of each doc in the BM25 formula.
        Vector<T> row_sums;

        // below are used only for WAND index.
        float max_score_ratio;
        DocValueComputer<T> wand_max_score_computer;

        BM25Params(float k1, float b, float avgdl, float max_score_ratio)
            : k1(k1),
              b(b),
              max_score_ratio(max_score_ratio),
              wand_max_score_computer(GetDocValueBM25Computer<T>(k1, b, avgdl)) {
        }
    };  // struct BM25Params

    std::unique_ptr<BM25Params> bm25_params_;

};  // class InvertedIndex

}  // namespace knowhere::sparse

#endif  // SPARSE_INVERTED_INDEX_H
