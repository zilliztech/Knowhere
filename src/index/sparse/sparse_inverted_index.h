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
#include <sys/types.h>
#include <unistd.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

#include "index/sparse/sparse_inverted_index_config.h"
#include "io/memory_io.h"
#include "knowhere/bitsetview.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/expected.h"
#include "knowhere/log.h"
#include "knowhere/sparse_utils.h"
#include "knowhere/utils.h"

namespace knowhere::sparse {

enum class InvertedIndexAlgo {
    TAAT_NAIVE,
    DAAT_WAND,
    DAAT_MAXSCORE,
    DAAT_BLOCKMAX_WAND,
    DAAT_BLOCKMAX_MAXSCORE,
};

struct InvertedIndexApproxSearchParams {
    int refine_factor;
    float drop_ratio_search;
    float dim_max_score_ratio;
};

template <typename T>
class BaseInvertedIndex {
 public:
    virtual ~BaseInvertedIndex() = default;

    virtual Status
    Save(MemoryIOWriter& writer) = 0;

    // supplement_target_filename: when in mmap mode, we need an extra file to store the mmapped index data structure.
    // this file will be created during loading and deleted in the destructor.
    virtual Status
    Load(MemoryIOReader& reader, int map_flags, const std::string& supplement_target_filename) = 0;

    virtual Status
    Train(const SparseRow<T>* data, size_t rows) = 0;

    virtual Status
    Add(const SparseRow<T>* data, size_t rows, int64_t dim) = 0;

    virtual void
    Search(const SparseRow<T>& query, size_t k, float* distances, label_t* labels, const BitsetView& bitset,
           const DocValueComputer<T>& computer, InvertedIndexApproxSearchParams& approx_params) const = 0;

    virtual std::vector<float>
    GetAllDistances(const SparseRow<T>& query, float drop_ratio_search, const BitsetView& bitset,
                    const DocValueComputer<T>& computer) const = 0;

    virtual float
    GetRawDistance(const label_t vec_id, const SparseRow<T>& query, const DocValueComputer<T>& computer) const = 0;

    virtual expected<DocValueComputer<T>>
    GetDocValueComputer(const SparseInvertedIndexConfig& cfg) const = 0;

    [[nodiscard]] virtual size_t
    size() const = 0;

    [[nodiscard]] virtual size_t
    n_rows() const = 0;

    [[nodiscard]] virtual size_t
    n_cols() const = 0;
};

template <typename DType, typename QType, InvertedIndexAlgo algo, bool mmapped = false>
class InvertedIndex : public BaseInvertedIndex<DType> {
 public:
    explicit InvertedIndex(SparseMetricType metric_type) : metric_type_(metric_type) {
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
            if (blockmax_map_ != nullptr) {
                auto res = munmap(blockmax_map_, blockmax_map_byte_size_);
                if (res != 0) {
                    LOG_KNOWHERE_ERROR_ << "Failed to munmap when deleting sparse InvertedIndex: " << strerror(errno);
                }
                blockmax_map_ = nullptr;
                blockmax_map_byte_size_ = 0;
            }
            if (blockmax_map_fd_ != -1) {
                // closing the file descriptor will also cause the file to be deleted.
                close(blockmax_map_fd_);
                blockmax_map_fd_ = -1;
            }
        }
    }

    template <typename U>
    using Vector = std::conditional_t<mmapped, GrowableVectorView<U>, std::vector<U>>;

    void
    SetBM25Params(float k1, float b, float avgdl) {
        bm25_params_ = std::make_unique<BM25Params>(k1, b, avgdl);
    }

    void
    SetBlockmaxBlockSize(size_t block_size) {
        blockmax_block_size_ = block_size;
    }

    expected<DocValueComputer<float>>
    GetDocValueComputer(const SparseInvertedIndexConfig& cfg) const override {
        // if metric_type is set in config, it must match with how the index was built.
        auto metric_type = cfg.metric_type;
        if (metric_type_ != SparseMetricType::METRIC_BM25) {
            if (metric_type.has_value() && !IsMetricType(metric_type.value(), metric::IP)) {
                auto msg =
                    "metric type not match, expected: " + std::string(metric::IP) + ", got: " + metric_type.value();
                return expected<DocValueComputer<float>>::Err(Status::invalid_metric_type, msg);
            }
            return GetDocValueOriginalComputer<float>();
        }
        if (metric_type.has_value() && !IsMetricType(metric_type.value(), metric::BM25)) {
            auto msg =
                "metric type not match, expected: " + std::string(metric::BM25) + ", got: " + metric_type.value();
            return expected<DocValueComputer<float>>::Err(Status::invalid_metric_type, msg);
        }
        // avgdl must be supplied during search
        if (!cfg.bm25_avgdl.has_value()) {
            return expected<DocValueComputer<float>>::Err(Status::invalid_args,
                                                          "avgdl must be supplied during searching");
        }
        auto avgdl = cfg.bm25_avgdl.value();
        if constexpr (algo != InvertedIndexAlgo::TAAT_NAIVE) {
            // daat related algorithms: search time k1/b must equal load time config.
            if ((cfg.bm25_k1.has_value() && cfg.bm25_k1.value() != bm25_params_->k1) ||
                ((cfg.bm25_b.has_value() && cfg.bm25_b.value() != bm25_params_->b))) {
                return expected<DocValueComputer<float>>::Err(
                    Status::invalid_args, "search time k1/b must equal load time config for DAAT_* algorithm.");
            }
            return GetDocValueBM25Computer<float>(bm25_params_->k1, bm25_params_->b, avgdl);
        } else {
            // inverted index: search time k1/b may override load time config.
            auto k1 = cfg.bm25_k1.has_value() ? cfg.bm25_k1.value() : bm25_params_->k1;
            auto b = cfg.bm25_b.has_value() ? cfg.bm25_b.value() : bm25_params_->b;
            return GetDocValueBM25Computer<float>(k1, b, avgdl);
        }
    }

    Status
    Save(MemoryIOWriter& writer) override {
        /**
         * Layout:
         *
         * 1. size_t rows
         * 2. size_t cols
         * 3. DType value_threshold_ (deprecated)
         * 4. for each row:
         *     1. size_t len
         *     2. for each non-zero value:
         *        1. table_t idx
         *        2. DType val (when QType is different from DType, the QType value of val is stored as a DType with
         *           precision loss)
         *
         * inverted_index_ids_, inverted_index_vals_ and max_score_in_dim_ are
         * not serialized, they will be constructed dynamically during
         * deserialization.
         *
         * Data are densely packed in serialized bytes and no padding is added.
         */
        DType deprecated_value_threshold = 0;
        writeBinaryPOD(writer, n_rows_internal_);
        writeBinaryPOD(writer, max_dim_);
        writeBinaryPOD(writer, deprecated_value_threshold);

        auto dim_map_reverse = std::unordered_map<uint32_t, table_t>();
        for (const auto& [dim, dim_id] : dim_map_) {
            dim_map_reverse[dim_id] = dim;
        }

        std::vector<size_t> row_sizes(n_rows_internal_, 0);
        for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
            for (const auto& id : inverted_index_ids_[i]) {
                row_sizes[id]++;
            }
        }

        std::vector<SparseRow<DType>> raw_rows(n_rows_internal_);
        for (size_t i = 0; i < n_rows_internal_; ++i) {
            raw_rows[i] = std::move(SparseRow<DType>(row_sizes[i]));
        }

        for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
            const auto& ids = inverted_index_ids_[i];
            const auto& vals = inverted_index_vals_[i];
            const auto dim = dim_map_reverse[i];
            for (size_t j = 0; j < ids.size(); ++j) {
                raw_rows[ids[j]].set_at(raw_rows[ids[j]].size() - row_sizes[ids[j]], dim, vals[j]);
                --row_sizes[ids[j]];
            }
        }

        for (table_t vec_id = 0; vec_id < n_rows_internal_; ++vec_id) {
            writeBinaryPOD(writer, raw_rows[vec_id].size());
            if (raw_rows[vec_id].size() > 0) {
                writer.write(raw_rows[vec_id].data(), raw_rows[vec_id].size() * SparseRow<DType>::element_size());
            }
        }

        return Status::success;
    }

    Status
    Load(MemoryIOReader& reader, int map_flags, const std::string& supplement_target_filename) override {
        DType deprecated_value_threshold;
        int64_t rows;
        readBinaryPOD(reader, rows);
        // previous versions used the signness of rows to indicate whether to
        // use wand. now we use a template parameter to control this thus simply
        // take the absolute value of rows.
        rows = std::abs(rows);
        readBinaryPOD(reader, max_dim_);
        readBinaryPOD(reader, deprecated_value_threshold);

        if constexpr (mmapped) {
            RETURN_IF_ERROR(prepare_index_mmap(reader, rows, map_flags, supplement_target_filename));
        } else {
            if (metric_type_ == SparseMetricType::METRIC_BM25) {
                bm25_params_->row_sums.reserve(rows);
            }
        }

        for (int64_t i = 0; i < rows; ++i) {
            size_t count;
            readBinaryPOD(reader, count);
            SparseRow<DType> raw_row;
            if constexpr (mmapped) {
                raw_row = std::move(SparseRow<DType>(count, reader.data() + reader.tellg(), false));
                reader.advance(count * SparseRow<DType>::element_size());
            } else {
                raw_row = std::move(SparseRow<DType>(count));
                if (count > 0) {
                    reader.read(raw_row.data(), count * SparseRow<DType>::element_size());
                }
            }
            add_row_to_index(raw_row, i, false);
        }

        n_rows_internal_ = rows;

        // prepare and generate blockmax information
        if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_MAXSCORE ||
                      algo == InvertedIndexAlgo::DAAT_BLOCKMAX_WAND) {
            if constexpr (mmapped) {
                RETURN_IF_ERROR(prepare_blockmax_mmap(map_flags, supplement_target_filename + ".blockmax"));
            } else {
                blockmax_last_block_sizes_.resize(inverted_index_ids_.size(), 0);
                blockmax_ids_.resize(inverted_index_ids_.size());
                blockmax_scores_.resize(inverted_index_ids_.size());
            }

            for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
                auto& ids = inverted_index_ids_[i];
                auto& vals = inverted_index_vals_[i];
                for (size_t j = 0; j < ids.size(); ++j) {
                    float score = static_cast<float>(vals[j]);
                    if (metric_type_ == SparseMetricType::METRIC_BM25) {
                        score = bm25_params_->max_score_computer(vals[j], bm25_params_->row_sums.at(ids[j]));
                    }
                    if (blockmax_last_block_sizes_[i] == 0) {
                        blockmax_ids_[i].emplace_back(ids[j]);
                        blockmax_scores_[i].emplace_back(score);
                    } else {
                        blockmax_ids_[i].back() = ids[j];
                        blockmax_scores_[i].back() = std::max(blockmax_scores_[i].back(), score);
                    }
                    if (++blockmax_last_block_sizes_[i] >= blockmax_block_size_) {
                        blockmax_last_block_sizes_[i] = 0;
                    }
                }
            }
        }

        return Status::success;
    }

    // memory in reader must be guaranteed to be valid during the lifetime of this object.
    Status
    prepare_index_mmap(MemoryIOReader& reader, size_t rows, int map_flags,
                       const std::string& supplement_target_filename) {
        const auto initial_reader_location = reader.tellg();
        const auto nnz = (reader.remaining() - (rows * sizeof(size_t))) / SparseRow<DType>::element_size();

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
                reader.advance(sizeof(DType));
            }
        }
        // reset reader to the beginning
        reader.seekg(initial_reader_location);

        auto inverted_index_ids_byte_size =
            idx_counts.size() * sizeof(typename decltype(inverted_index_ids_)::value_type);
        auto inverted_index_vals_byte_size =
            idx_counts.size() * sizeof(typename decltype(inverted_index_vals_)::value_type);
        auto plists_ids_byte_size = nnz * sizeof(typename decltype(inverted_index_ids_)::value_type::value_type);
        auto plists_vals_byte_size = nnz * sizeof(typename decltype(inverted_index_vals_)::value_type::value_type);
        auto max_score_in_dim_byte_size = idx_counts.size() * sizeof(typename decltype(max_score_in_dim_)::value_type);
        size_t row_sums_byte_size = 0;

        map_byte_size_ =
            inverted_index_ids_byte_size + inverted_index_vals_byte_size + plists_ids_byte_size + plists_vals_byte_size;
        if constexpr (algo != InvertedIndexAlgo::TAAT_NAIVE) {
            map_byte_size_ += max_score_in_dim_byte_size;
        }
        if (metric_type_ == SparseMetricType::METRIC_BM25) {
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
        inverted_index_ids_.initialize(ptr, inverted_index_ids_byte_size);
        ptr += inverted_index_ids_byte_size;
        inverted_index_vals_.initialize(ptr, inverted_index_vals_byte_size);
        ptr += inverted_index_vals_byte_size;

        if constexpr (algo != InvertedIndexAlgo::TAAT_NAIVE) {
            max_score_in_dim_.initialize(ptr, max_score_in_dim_byte_size);
            ptr += max_score_in_dim_byte_size;
        }

        if (metric_type_ == SparseMetricType::METRIC_BM25) {
            bm25_params_->row_sums.initialize(ptr, row_sums_byte_size);
            ptr += row_sums_byte_size;
        }

        for (const auto& [idx, count] : idx_counts) {
            auto& plist_ids = inverted_index_ids_.emplace_back();
            auto plist_ids_byte_size = count * sizeof(typename decltype(inverted_index_ids_)::value_type::value_type);
            plist_ids.initialize(ptr, plist_ids_byte_size);
            ptr += plist_ids_byte_size;
        }
        for (const auto& [idx, count] : idx_counts) {
            auto& plist_vals = inverted_index_vals_.emplace_back();
            auto plist_vals_byte_size = count * sizeof(typename decltype(inverted_index_vals_)::value_type::value_type);
            plist_vals.initialize(ptr, plist_vals_byte_size);
            ptr += plist_vals_byte_size;
        }
        size_t dim_id = 0;
        for (const auto& [idx, count] : idx_counts) {
            dim_map_[idx] = dim_id;
            if constexpr (algo != InvertedIndexAlgo::TAAT_NAIVE) {
                max_score_in_dim_.emplace_back(0.0f);
            }
            ++dim_id;
        }

        // in mmap mode, next_dim_id_ should never be used, but still assigning for consistency.
        next_dim_id_ = dim_id;

        return Status::success;
    }

    Status
    prepare_blockmax_mmap(int map_flags, const std::string& blockmax_mmap_filename) {
        std::ofstream temp_file(blockmax_mmap_filename, std::ios::binary | std::ios::trunc);
        if (!temp_file) {
            LOG_KNOWHERE_ERROR_ << "Failed to create mmap file when loading sparse InvertedIndex: " << strerror(errno);
            return Status::disk_file_error;
        }
        temp_file.close();

        size_t blockmax_total_blocks = 0;
        for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
            blockmax_total_blocks += (inverted_index_ids_[i].size() + blockmax_block_size_ - 1) / blockmax_block_size_;
        }
        auto blockmax_last_block_sizes_byte_size =
            inverted_index_ids_.size() * sizeof(typename decltype(blockmax_last_block_sizes_)::value_type);
        auto blockmax_ids_dim0_byte_size =
            inverted_index_ids_.size() * sizeof(typename decltype(blockmax_ids_)::value_type);
        auto blockmax_scores_dim0_byte_size =
            inverted_index_ids_.size() * sizeof(typename decltype(blockmax_scores_)::value_type);
        auto blockmax_total_blocks_byte_size =
            blockmax_total_blocks * (sizeof(typename decltype(blockmax_ids_)::value_type::value_type) +
                                     sizeof(typename decltype(blockmax_scores_)::value_type::value_type));
        blockmax_map_byte_size_ = blockmax_last_block_sizes_byte_size + blockmax_ids_dim0_byte_size +
                                  blockmax_scores_dim0_byte_size + blockmax_total_blocks_byte_size;
        std::filesystem::resize_file(blockmax_mmap_filename, blockmax_map_byte_size_);

        blockmax_map_fd_ = open(blockmax_mmap_filename.c_str(), O_RDWR);
        if (blockmax_map_fd_ == -1) {
            LOG_KNOWHERE_ERROR_ << "Failed to open mmap file when loading sparse InvertedIndex: " << strerror(errno);
            return Status::disk_file_error;
        }
        std::filesystem::remove(blockmax_mmap_filename);
        // clear MAP_PRIVATE flag: we need to write to this mmapped memory/file,
        // MAP_PRIVATE triggers copy-on-write and uses extra anonymous memory.
        map_flags &= ~MAP_PRIVATE;
        map_flags |= MAP_SHARED;
        blockmax_map_ = static_cast<char*>(
            mmap(nullptr, blockmax_map_byte_size_, PROT_READ | PROT_WRITE, map_flags, blockmax_map_fd_, 0));
        if (blockmax_map_ == MAP_FAILED) {
            LOG_KNOWHERE_ERROR_ << "Failed to create blockmax mmap when loading sparse InvertedIndex: "
                                << strerror(errno) << ", size: " << blockmax_map_byte_size_
                                << " on file: " << blockmax_mmap_filename;
            return Status::disk_file_error;
        }

        char* ptr = blockmax_map_;

        blockmax_last_block_sizes_.initialize(ptr, blockmax_last_block_sizes_byte_size);
        ptr += blockmax_last_block_sizes_byte_size;
        blockmax_ids_.initialize(ptr, blockmax_ids_dim0_byte_size);
        ptr += blockmax_ids_dim0_byte_size;
        blockmax_scores_.initialize(ptr, blockmax_scores_dim0_byte_size);
        ptr += blockmax_scores_dim0_byte_size;

        for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
            blockmax_last_block_sizes_[i] = 0;
        }
        for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
            auto bcount = (inverted_index_ids_[i].size() + blockmax_block_size_ - 1) / blockmax_block_size_;
            auto& bids = blockmax_ids_.emplace_back();
            bids.initialize(ptr, bcount * sizeof(typename decltype(blockmax_ids_)::value_type::value_type));
            ptr += bcount * sizeof(typename decltype(blockmax_ids_)::value_type::value_type);
        }
        for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
            auto bcount = (inverted_index_ids_[i].size() + blockmax_block_size_ - 1) / blockmax_block_size_;
            auto& bscores = blockmax_scores_.emplace_back();
            bscores.initialize(ptr, bcount * sizeof(typename decltype(blockmax_scores_)::value_type::value_type));
            ptr += bcount * sizeof(typename decltype(blockmax_scores_)::value_type::value_type);
        }

        return Status::success;
    }

    // Non zero drop ratio is only supported for static index, i.e. data should
    // include all rows that'll be added to the index.
    Status
    Train(const SparseRow<DType>* data, size_t rows) override {
        if constexpr (mmapped) {
            throw std::invalid_argument("mmapped InvertedIndex does not support Train");
        } else {
            return Status::success;
        }
    }

    Status
    Add(const SparseRow<DType>* data, size_t rows, int64_t dim) override {
        if constexpr (mmapped) {
            throw std::invalid_argument("mmapped InvertedIndex does not support Add");
        } else {
            auto current_rows = n_rows_internal_;
            if ((size_t)dim > max_dim_) {
                max_dim_ = dim;
            }

            if (metric_type_ == SparseMetricType::METRIC_BM25) {
                bm25_params_->row_sums.reserve(current_rows + rows);
            }
            for (size_t i = 0; i < rows; ++i) {
                add_row_to_index(data[i], current_rows + i, true);
            }

            n_rows_internal_ += rows;

            return Status::success;
        }
    }

    void
    Search(const SparseRow<DType>& query, size_t k, float* distances, label_t* labels, const BitsetView& bitset,
           const DocValueComputer<float>& computer, InvertedIndexApproxSearchParams& approx_params) const override {
        // initially set result distances to NaN and labels to -1
        std::fill(distances, distances + k, std::numeric_limits<float>::quiet_NaN());
        std::fill(labels, labels + k, -1);
        if (query.size() == 0) {
            return;
        }

        auto q_vec = parse_query(query, approx_params.drop_ratio_search);
        if (q_vec.empty()) {
            return;
        }

        MaxMinHeap<float> heap(k * approx_params.refine_factor);
        // DAAT related algorithms are based on the implementation in PISA.
        if constexpr (algo == InvertedIndexAlgo::DAAT_WAND) {
            search_daat_wand(q_vec, heap, bitset, computer, approx_params.dim_max_score_ratio);
        } else if constexpr (algo == InvertedIndexAlgo::DAAT_MAXSCORE) {
            search_daat_maxscore(q_vec, heap, bitset, computer, approx_params.dim_max_score_ratio);
        } else if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_MAXSCORE) {
            search_daat_blockmax_maxscore(q_vec, heap, bitset, computer, approx_params.dim_max_score_ratio);
        } else if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_WAND) {
            search_daat_blockmax_wand(q_vec, heap, bitset, computer, approx_params.dim_max_score_ratio);
        } else {
            search_taat_naive(q_vec, heap, bitset, computer);
        }

        if (approx_params.refine_factor == 1) {
            collect_result(heap, distances, labels);
        } else {
            refine_and_collect(query, heap, k, distances, labels, computer, approx_params);
        }
    }

    // Returned distances are inaccurate based on the drop_ratio.
    std::vector<float>
    GetAllDistances(const SparseRow<DType>& query, float drop_ratio_search, const BitsetView& bitset,
                    const DocValueComputer<float>& computer) const override {
        if (query.size() == 0) {
            return {};
        }
        std::vector<DType> values(query.size());
        for (size_t i = 0; i < query.size(); ++i) {
            values[i] = std::abs(query[i].val);
        }
        auto q_vec = parse_query(query, drop_ratio_search);

        auto distances = compute_all_distances(q_vec, computer);
        if (!bitset.empty()) {
            for (size_t i = 0; i < distances.size(); ++i) {
                if (bitset.test(i)) {
                    distances[i] = 0.0f;
                }
            }
        }
        return distances;
    }

    float
    GetRawDistance(const label_t vec_id, const SparseRow<DType>& query,
                   const DocValueComputer<float>& computer) const override {
        float distance = 0.0f;

        for (size_t i = 0; i < query.size(); ++i) {
            auto [dim, val] = query[i];
            auto dim_it = dim_map_.find(dim);
            if (dim_it == dim_map_.cend()) {
                continue;
            }
            auto& plist_ids = inverted_index_ids_[dim_it->second];
            auto it = std::lower_bound(plist_ids.begin(), plist_ids.end(), vec_id,
                                       [](const auto& x, table_t y) { return x < y; });
            if (it != plist_ids.end() && *it == vec_id) {
                distance +=
                    val *
                    computer(inverted_index_vals_[dim_it->second][it - plist_ids.begin()],
                             metric_type_ == SparseMetricType::METRIC_BM25 ? bm25_params_->row_sums.at(vec_id) : 0);
            }
        }

        return distance;
    }

    [[nodiscard]] size_t
    size() const override {
        size_t res = sizeof(*this);
        res += dim_map_.size() *
               (sizeof(typename decltype(dim_map_)::key_type) + sizeof(typename decltype(dim_map_)::mapped_type));

        if constexpr (mmapped) {
            return res + map_byte_size_;
        } else {
            res += sizeof(typename decltype(inverted_index_ids_)::value_type) * inverted_index_ids_.capacity();
            for (size_t i = 0; i < inverted_index_ids_.size(); ++i) {
                res += sizeof(typename decltype(inverted_index_ids_)::value_type::value_type) *
                       inverted_index_ids_[i].capacity();
            }
            res += sizeof(typename decltype(inverted_index_vals_)::value_type) * inverted_index_vals_.capacity();
            for (size_t i = 0; i < inverted_index_vals_.size(); ++i) {
                res += sizeof(typename decltype(inverted_index_vals_)::value_type::value_type) *
                       inverted_index_vals_[i].capacity();
            }
            if constexpr (algo != InvertedIndexAlgo::TAAT_NAIVE) {
                res += sizeof(typename decltype(max_score_in_dim_)::value_type) * max_score_in_dim_.capacity();
            }
            if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_WAND ||
                          algo == InvertedIndexAlgo::DAAT_BLOCKMAX_MAXSCORE) {
                res += sizeof(typename decltype(blockmax_ids_)::value_type) * blockmax_ids_.capacity();
                res += sizeof(typename decltype(blockmax_scores_)::value_type) * blockmax_scores_.capacity();
                for (size_t i = 0; i < blockmax_ids_.size(); ++i) {
                    res +=
                        sizeof(typename decltype(blockmax_ids_)::value_type::value_type) * blockmax_ids_[i].capacity();
                }
                for (size_t i = 0; i < blockmax_scores_.size(); ++i) {
                    res += sizeof(typename decltype(blockmax_scores_)::value_type::value_type) *
                           blockmax_scores_[i].capacity();
                }
            }
            return res;
        }
    }

    [[nodiscard]] size_t
    n_rows() const override {
        return n_rows_internal_;
    }

    [[nodiscard]] size_t
    n_cols() const override {
        return max_dim_;
    }

 private:
    // Given a vector of values, returns the threshold value.
    // All values strictly smaller than the threshold will be ignored.
    // values will be modified in this function.
    inline DType
    get_threshold(std::vector<DType>& values, float drop_ratio) const {
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

    std::vector<float>
    compute_all_distances(const std::vector<std::pair<size_t, DType>>& q_vec,
                          const DocValueComputer<float>& computer) const {
        std::vector<float> scores(n_rows_internal_, 0.0f);
        for (size_t i = 0; i < q_vec.size(); ++i) {
            auto& plist_ids = inverted_index_ids_[q_vec[i].first];
            auto& plist_vals = inverted_index_vals_[q_vec[i].first];
            // TODO: improve with SIMD
            for (size_t j = 0; j < plist_ids.size(); ++j) {
                auto doc_id = plist_ids[j];
                float val_sum = metric_type_ == SparseMetricType::METRIC_BM25 ? bm25_params_->row_sums.at(doc_id) : 0;
                scores[doc_id] += q_vec[i].second * computer(plist_vals[j], val_sum);
            }
        }
        return scores;
    }

    template <typename DocIdFilter>
    struct Cursor {
     public:
        Cursor(const Vector<table_t>& plist_ids, const Vector<QType>& plist_vals, size_t num_vec, float max_score,
               float q_value, DocIdFilter filter)
            : plist_ids_(plist_ids),
              plist_vals_(plist_vals),
              plist_size_(plist_ids.size()),
              total_num_vec_(num_vec),
              max_score_(max_score),
              q_value_(q_value),
              filter_(filter) {
            skip_filtered_ids();
            update_cur_vec_id();
        }
        Cursor(const Cursor& rhs) = delete;
        Cursor(Cursor&& rhs) noexcept = default;

        void
        next() {
            ++loc_;
            skip_filtered_ids();
            update_cur_vec_id();
        }

        void
        seek(table_t vec_id) {
            while (loc_ < plist_size_ && plist_ids_[loc_] < vec_id) {
                ++loc_;
            }
            skip_filtered_ids();
            update_cur_vec_id();
        }

        QType
        cur_vec_val() const {
            return plist_vals_[loc_];
        }

        const Vector<table_t>& plist_ids_;
        const Vector<QType>& plist_vals_;
        const size_t plist_size_;
        size_t loc_ = 0;
        size_t total_num_vec_ = 0;
        float max_score_ = 0.0f;
        float q_value_ = 0.0f;
        DocIdFilter filter_;
        table_t cur_vec_id_ = 0;

     private:
        inline void
        update_cur_vec_id() {
            cur_vec_id_ = (loc_ >= plist_size_) ? total_num_vec_ : plist_ids_[loc_];
        }

        inline void
        skip_filtered_ids() {
            while (loc_ < plist_size_ && !filter_.empty() && filter_.test(plist_ids_[loc_])) {
                ++loc_;
            }
        }
    };  // struct Cursor

    template <typename DocIdFilter>
    struct BlockMaxCursor : public Cursor<DocIdFilter> {
     public:
        BlockMaxCursor(const Vector<table_t>& plist_ids, const Vector<QType>& plist_vals,
                       const Vector<table_t>& pblk_max_ids, const Vector<float>& pblk_max_scores, size_t num_vec,
                       float max_score, float q_value, DocIdFilter filter, float block_max_score_ratio)
            : Cursor<DocIdFilter>(plist_ids, plist_vals, num_vec, max_score, q_value, filter),
              pblk_max_ids_(pblk_max_ids),
              pblk_max_scores_(pblk_max_scores),
              scaled_q_value_(q_value * block_max_score_ratio) {
        }
        void
        seek_block(table_t vec_id) {
            while (bloc_ < pblk_max_ids_.size() && pblk_max_ids_[bloc_] < vec_id) {
                ++bloc_;
            }
            cur_block_end_vec_id_ = (bloc_ >= pblk_max_ids_.size()) ? this->total_num_vec_ : pblk_max_ids_[bloc_];
        }

        [[nodiscard]] float
        cur_block_max_score() const {
            if (bloc_ >= pblk_max_scores_.size()) {
                return 0;
            }
            return scaled_q_value_ * pblk_max_scores_[bloc_];
        }

        const Vector<table_t>& pblk_max_ids_;
        const Vector<float>& pblk_max_scores_;
        float scaled_q_value_ = 0.0f;
        table_t cur_block_end_vec_id_ = 0;
        size_t bloc_ = 0;
    };  // struct BlockMaxCursor

    std::vector<std::pair<size_t, DType>>
    parse_query(const SparseRow<DType>& query, float drop_ratio_search) const {
        DType q_threshold = 0;
        if (drop_ratio_search != 0) {
            std::vector<DType> values(query.size());
            for (size_t i = 0; i < query.size(); ++i) {
                values[i] = std::abs(query[i].val);
            }
            q_threshold = get_threshold(values, drop_ratio_search);
        }

        std::vector<std::pair<size_t, DType>> filtered_query;
        for (size_t i = 0; i < query.size(); ++i) {
            auto [dim, val] = query[i];
            auto dim_it = dim_map_.find(dim);
            if (dim_it == dim_map_.cend() || std::abs(val) < q_threshold) {
                continue;
            }
            filtered_query.emplace_back(dim_it->second, val);
        }

        return filtered_query;
    }

    template <typename DocIdFilter>
    std::vector<Cursor<DocIdFilter>>
    make_cursors(const std::vector<std::pair<size_t, DType>>& q_vec, const DocValueComputer<float>& computer,
                 DocIdFilter& filter, float dim_max_score_ratio) const {
        std::vector<Cursor<DocIdFilter>> cursors;
        cursors.reserve(q_vec.size());
        for (auto q_dim : q_vec) {
            auto& plist_ids = inverted_index_ids_[q_dim.first];
            auto& plist_vals = inverted_index_vals_[q_dim.first];
            cursors.emplace_back(plist_ids, plist_vals, n_rows_internal_,
                                 max_score_in_dim_[q_dim.first] * q_dim.second * dim_max_score_ratio, q_dim.second,
                                 filter);
        }
        return cursors;
    }

    template <typename DocIdFilter>
    std::vector<BlockMaxCursor<DocIdFilter>>
    make_blockmax_cursors(const std::vector<std::pair<size_t, DType>>& q_vec, const DocValueComputer<float>& computer,
                          DocIdFilter& filter, float dim_max_score_ratio, float block_max_score_ratio = 1.05) const {
        std::vector<BlockMaxCursor<DocIdFilter>> cursors;
        cursors.reserve(q_vec.size());
        for (auto q : q_vec) {
            auto& plist_ids = inverted_index_ids_[q.first];
            auto& plist_vals = inverted_index_vals_[q.first];
            auto& pblk_max_ids = blockmax_ids_[q.first];
            auto& pblk_max_scores = blockmax_scores_[q.first];
            cursors.emplace_back(plist_ids, plist_vals, pblk_max_ids, pblk_max_scores, n_rows_internal_,
                                 max_score_in_dim_[q.first] * q.second * dim_max_score_ratio, q.second, filter,
                                 block_max_score_ratio);
        }
        return cursors;
    }

    // find the top-k candidates using brute force search, k as specified by the capacity of the heap.
    // any value in q_vec that is smaller than q_threshold and any value with dimension >= n_cols() will be ignored.
    // TODO: may switch to row-wise brute force if filter rate is high. Benchmark needed.
    template <typename DocIdFilter>
    void
    search_taat_naive(const std::vector<std::pair<size_t, DType>>& q_vec, MaxMinHeap<float>& heap, DocIdFilter& filter,
                      const DocValueComputer<float>& computer) const {
        auto scores = compute_all_distances(q_vec, computer);
        for (size_t i = 0; i < n_rows_internal_; ++i) {
            if ((filter.empty() || !filter.test(i)) && scores[i] != 0) {
                heap.push(i, scores[i]);
            }
        }
    }

    template <typename DocIdFilter>
    void
    search_daat_wand(const std::vector<std::pair<size_t, DType>>& q_vec, MaxMinHeap<float>& heap, DocIdFilter& filter,
                     const DocValueComputer<float>& computer, float dim_max_score_ratio) const {
        std::vector<Cursor<DocIdFilter>> cursors = make_cursors(q_vec, computer, filter, dim_max_score_ratio);
        std::vector<Cursor<DocIdFilter>*> cursor_ptrs(cursors.size());
        for (size_t i = 0; i < cursors.size(); ++i) {
            cursor_ptrs[i] = &cursors[i];
        }

        auto sort_cursors = [&cursor_ptrs] {
            std::sort(cursor_ptrs.begin(), cursor_ptrs.end(),
                      [](auto& x, auto& y) { return x->cur_vec_id_ < y->cur_vec_id_; });
        };
        sort_cursors();

        while (true) {
            float threshold = heap.full() ? heap.top().val : 0;
            float upper_bound = 0;
            size_t pivot;

            bool found_pivot = false;
            for (pivot = 0; pivot < q_vec.size(); ++pivot) {
                if (cursor_ptrs[pivot]->cur_vec_id_ >= n_rows_internal_) {
                    break;
                }
                upper_bound += cursor_ptrs[pivot]->max_score_;
                if (upper_bound > threshold) {
                    found_pivot = true;
                    break;
                }
            }
            if (!found_pivot) {
                break;
            }

            table_t pivot_id = cursor_ptrs[pivot]->cur_vec_id_;
            if (pivot_id == cursor_ptrs[0]->cur_vec_id_) {
                float score = 0;
                float cur_vec_sum =
                    metric_type_ == SparseMetricType::METRIC_BM25 ? bm25_params_->row_sums.at(pivot_id) : 0;
                for (auto& cursor_ptr : cursor_ptrs) {
                    if (cursor_ptr->cur_vec_id_ != pivot_id) {
                        break;
                    }
                    score += cursor_ptr->q_value_ * computer(cursor_ptr->cur_vec_val(), cur_vec_sum);
                    cursor_ptr->next();
                }
                heap.push(pivot_id, score);
                sort_cursors();
            } else {
                size_t next_list = pivot;
                for (; cursor_ptrs[next_list]->cur_vec_id_ == pivot_id; --next_list) {
                }
                cursor_ptrs[next_list]->seek(pivot_id);
                for (size_t i = next_list + 1; i < q_vec.size(); ++i) {
                    if (cursor_ptrs[i]->cur_vec_id_ >= cursor_ptrs[i - 1]->cur_vec_id_) {
                        break;
                    }
                    std::swap(cursor_ptrs[i], cursor_ptrs[i - 1]);
                }
            }
        }
    }

    template <typename DocIdFilter>
    void
    search_daat_maxscore(std::vector<std::pair<size_t, DType>>& q_vec, MaxMinHeap<float>& heap, DocIdFilter& filter,
                         const DocValueComputer<float>& computer, float dim_max_score_ratio) const {
        std::sort(q_vec.begin(), q_vec.end(), [this](auto& a, auto& b) {
            return a.second * max_score_in_dim_[a.first] > b.second * max_score_in_dim_[b.first];
        });

        std::vector<Cursor<DocIdFilter>> cursors = make_cursors(q_vec, computer, filter, dim_max_score_ratio);

        float threshold = heap.full() ? heap.top().val : 0;

        std::vector<float> upper_bounds(cursors.size());
        float bound_sum = 0.0;
        for (size_t i = cursors.size() - 1; i + 1 > 0; --i) {
            bound_sum += cursors[i].max_score_;
            upper_bounds[i] = bound_sum;
        }

        table_t next_cand_vec_id = n_rows_internal_;
        for (size_t i = 0; i < cursors.size(); ++i) {
            if (cursors[i].cur_vec_id_ < next_cand_vec_id) {
                next_cand_vec_id = cursors[i].cur_vec_id_;
            }
        }

        // first_ne_idx is the index of the first non-essential cursor
        size_t first_ne_idx = cursors.size();

        while (first_ne_idx != 0 && upper_bounds[first_ne_idx - 1] <= threshold) {
            --first_ne_idx;
            if (first_ne_idx == 0) {
                return;
            }
        }

        float curr_cand_score = 0.0f;
        table_t curr_cand_vec_id = 0;

        while (curr_cand_vec_id < n_rows_internal_) {
            auto found_cand = false;
            while (found_cand == false) {
                // start find from next_vec_id
                if (next_cand_vec_id >= n_rows_internal_) {
                    return;
                }
                // get current candidate vector
                curr_cand_vec_id = next_cand_vec_id;
                curr_cand_score = 0.0f;
                // update next_cand_vec_id
                next_cand_vec_id = n_rows_internal_;
                float cur_vec_sum =
                    metric_type_ == SparseMetricType::METRIC_BM25 ? bm25_params_->row_sums.at(curr_cand_vec_id) : 0;

                for (size_t i = 0; i < first_ne_idx; ++i) {
                    if (cursors[i].cur_vec_id_ == curr_cand_vec_id) {
                        curr_cand_score += cursors[i].q_value_ * computer(cursors[i].cur_vec_val(), cur_vec_sum);
                        cursors[i].next();
                    }
                    if (cursors[i].cur_vec_id_ < next_cand_vec_id) {
                        next_cand_vec_id = cursors[i].cur_vec_id_;
                    }
                }

                found_cand = true;
                for (size_t i = first_ne_idx; i < cursors.size(); ++i) {
                    if (curr_cand_score + upper_bounds[i] <= threshold) {
                        found_cand = false;
                        break;
                    }
                    cursors[i].seek(curr_cand_vec_id);
                    if (cursors[i].cur_vec_id_ == curr_cand_vec_id) {
                        curr_cand_score += cursors[i].q_value_ * computer(cursors[i].cur_vec_val(), cur_vec_sum);
                    }
                }
            }

            if (curr_cand_score > threshold) {
                heap.push(curr_cand_vec_id, curr_cand_score);
                threshold = heap.full() ? heap.top().val : 0;
                while (first_ne_idx != 0 && upper_bounds[first_ne_idx - 1] <= threshold) {
                    --first_ne_idx;
                    if (first_ne_idx == 0) {
                        return;
                    }
                }
            }
        }
    }

    template <typename DocIdFilter>
    void
    search_daat_blockmax_wand(std::vector<std::pair<size_t, DType>>& q_vec, MaxMinHeap<float>& heap,
                              DocIdFilter& filter, const DocValueComputer<float>& computer,
                              float dim_max_score_ratio) const {
        std::vector<BlockMaxCursor<DocIdFilter>> cursors =
            make_blockmax_cursors(q_vec, computer, filter, dim_max_score_ratio);
        std::vector<BlockMaxCursor<DocIdFilter>*> cursor_ptrs(cursors.size());
        for (size_t i = 0; i < cursors.size(); ++i) {
            cursor_ptrs[i] = &cursors[i];
        }

        auto sort_cursors = [&cursor_ptrs] {
            std::sort(cursor_ptrs.begin(), cursor_ptrs.end(),
                      [](auto& x, auto& y) { return x->cur_vec_id_ < y->cur_vec_id_; });
        };
        sort_cursors();

        while (true) {
            float threshold = heap.full() ? heap.top().val : 0;
            float global_upper_bound = 0;
            table_t pivot_id;
            size_t pivot;
            bool found_pivot = false;

            for (pivot = 0; pivot < cursor_ptrs.size(); ++pivot) {
                if (cursor_ptrs[pivot]->cur_vec_id_ >= n_rows_internal_) {
                    break;
                }
                global_upper_bound += cursor_ptrs[pivot]->max_score_;
                if (global_upper_bound > threshold) {
                    found_pivot = true;
                    pivot_id = cursor_ptrs[pivot]->cur_vec_id_;
                    for (; pivot + 1 < cursor_ptrs.size(); ++pivot) {
                        if (cursor_ptrs[pivot + 1]->cur_vec_id_ != pivot_id) {
                            break;
                        }
                    }
                    break;
                }
            }

            if (!found_pivot) {
                break;
            }

            float block_upper_bound = 0.0f;
            for (size_t i = 0; i <= pivot; ++i) {
                if (cursor_ptrs[i]->cur_block_end_vec_id_ < pivot_id) {
                    cursor_ptrs[i]->seek_block(pivot_id);
                }
                block_upper_bound += cursor_ptrs[i]->cur_block_max_score();
            }

            if (block_upper_bound > threshold) {
                if (pivot_id == cursor_ptrs[0]->cur_vec_id_) {
                    float score = 0.0f;
                    float cur_vec_sum =
                        metric_type_ == SparseMetricType::METRIC_BM25 ? bm25_params_->row_sums.at(pivot_id) : 0;
                    for (auto& cursor_ptr : cursor_ptrs) {
                        if (cursor_ptr->cur_vec_id_ != pivot_id) {
                            break;
                        }
                        score += cursor_ptr->q_value_ * computer(cursor_ptr->cur_vec_val(), cur_vec_sum);
                        cursor_ptr->next();
                    }

                    heap.push(pivot_id, score);
                    threshold = heap.full() ? heap.top().val : 0;
                    sort_cursors();
                } else {
                    size_t next_list = pivot;
                    for (; cursor_ptrs[next_list]->cur_vec_id_ == pivot_id; --next_list) {
                    }

                    cursor_ptrs[next_list]->seek(pivot_id);
                    for (size_t i = next_list + 1; i < cursor_ptrs.size(); ++i) {
                        if (cursor_ptrs[i]->cur_vec_id_ >= cursor_ptrs[i - 1]->cur_vec_id_) {
                            break;
                        }
                        std::swap(cursor_ptrs[i], cursor_ptrs[i - 1]);
                    }
                }
            } else {
                table_t cand_id = n_rows_internal_ - 1;
                for (size_t i = 0; i <= pivot; ++i) {
                    if (cursor_ptrs[i]->cur_block_end_vec_id_ < cand_id) {
                        cand_id = cursor_ptrs[i]->cur_block_end_vec_id_;
                    }
                }
                ++cand_id;

                // cursor_ptrs[pivot + 1] must have a vec_id greater than pivot_id,
                // and if this condition is met, it means pivot can start from pivot + 1
                if (pivot + 1 < cursor_ptrs.size() && cursor_ptrs[pivot + 1]->cur_vec_id_ < cand_id) {
                    cand_id = cursor_ptrs[pivot + 1]->cur_vec_id_;
                }
                assert(cand_id > pivot_id);

                size_t next_list = pivot;
                while (next_list + 1 > 0) {
                    cursor_ptrs[next_list]->seek(cand_id);
                    if (cursor_ptrs[next_list]->cur_vec_id_ != cand_id) {
                        for (size_t i = next_list + 1; i < cursor_ptrs.size(); ++i) {
                            if (cursor_ptrs[i]->cur_vec_id_ >= cursor_ptrs[i - 1]->cur_vec_id_) {
                                break;
                            }
                            std::swap(cursor_ptrs[i], cursor_ptrs[i - 1]);
                        }
                        break;
                    }
                    --next_list;
                }
            }
        }
    }

    template <typename DocIdFilter>
    void
    search_daat_blockmax_maxscore(std::vector<std::pair<size_t, DType>>& q_vec, MaxMinHeap<float>& heap,
                                  DocIdFilter& filter, const DocValueComputer<float>& computer,
                                  float dim_max_score_ratio) const {
        std::sort(q_vec.begin(), q_vec.end(), [this](auto& a, auto& b) {
            return a.second * max_score_in_dim_[a.first] > b.second * max_score_in_dim_[b.first];
        });
        std::vector<BlockMaxCursor<DocIdFilter>> cursors =
            make_blockmax_cursors(q_vec, computer, filter, dim_max_score_ratio);

        std::vector<float> upper_bounds(cursors.size() + 1, 0.0f);
        float bound_sum = 0.0f;
        for (size_t i = cursors.size() - 1; i + 1 > 0; --i) {
            bound_sum += cursors[i].max_score_;
            upper_bounds[i] = bound_sum;
        }

        float threshold = heap.full() ? heap.top().val : 0;

        table_t ne_start_cursor_id = cursors.size();
        uint64_t curr_cand_vec_id = (*std::min_element(cursors.begin(), cursors.end(), [](auto&& lhs, auto&& rhs) {
                                        return lhs.cur_vec_id_ < rhs.cur_vec_id_;
                                    })).cur_vec_id_;

        while (ne_start_cursor_id > 0 && curr_cand_vec_id < n_rows_internal_) {
            float score = 0;
            table_t next_cand_vec_id = n_rows_internal_;
            float cur_vec_sum =
                metric_type_ == SparseMetricType::METRIC_BM25 ? bm25_params_->row_sums.at(curr_cand_vec_id) : 0;

            // score essential list and find next
            for (size_t i = 0; i < ne_start_cursor_id; ++i) {
                if (cursors[i].cur_vec_id_ == curr_cand_vec_id) {
                    score += cursors[i].q_value_ * computer(cursors[i].cur_vec_val(), cur_vec_sum);
                    cursors[i].next();
                }
                if (cursors[i].cur_vec_id_ < next_cand_vec_id) {
                    next_cand_vec_id = cursors[i].cur_vec_id_;
                }
            }

            auto new_score = score + upper_bounds[ne_start_cursor_id];
            if (new_score > threshold) {
                // update block index for non-essential list and check block upper bound
                for (size_t i = ne_start_cursor_id; i < cursors.size(); ++i) {
                    if (cursors[i].cur_block_end_vec_id_ < curr_cand_vec_id) {
                        cursors[i].seek_block(curr_cand_vec_id);
                    }
                    new_score -= cursors[i].max_score_ - cursors[i].cur_block_max_score();
                    if (new_score <= threshold) {
                        break;
                    }
                }
                if (new_score > threshold) {
                    // try to complete evaluation with non-essential lists
                    for (size_t i = ne_start_cursor_id; i < cursors.size(); ++i) {
                        cursors[i].seek(curr_cand_vec_id);
                        if (cursors[i].cur_vec_id_ == curr_cand_vec_id) {
                            new_score += cursors[i].q_value_ * computer(cursors[i].cur_vec_val(), cur_vec_sum);
                        }
                        new_score -= cursors[i].cur_block_max_score();

                        if (new_score <= threshold) {
                            break;
                        }
                    }
                    score = new_score;
                }
                if (score > threshold) {
                    heap.push(curr_cand_vec_id, score);
                    threshold = heap.full() ? heap.top().val : 0;
                    // update non-essential lists
                    while (ne_start_cursor_id != 0 && upper_bounds[ne_start_cursor_id - 1] <= threshold) {
                        --ne_start_cursor_id;
                    }
                }
            }

            curr_cand_vec_id = next_cand_vec_id;
        }
    }

    void
    refine_and_collect(const SparseRow<DType>& query, MaxMinHeap<float>& inacc_heap, size_t k, float* distances,
                       label_t* labels, const DocValueComputer<float>& computer,
                       InvertedIndexApproxSearchParams& approx_params) const {
        std::vector<table_t> docids;
        MaxMinHeap<float> heap(k);

        docids.reserve(inacc_heap.size());
        while (!inacc_heap.empty()) {
            table_t u = inacc_heap.pop();
            docids.emplace_back(u);
        }

        auto q_vec = parse_query(query, 0);
        if (q_vec.empty()) {
            return;
        }

        // dim_max_score_ratio for refine process should be >= 1.0
        float dim_max_score_ratio = std::max(approx_params.dim_max_score_ratio, 1.0f);

        DocIdFilterByVector filter(std::move(docids));
        if constexpr (algo == InvertedIndexAlgo::DAAT_WAND) {
            search_daat_wand(q_vec, heap, filter, computer, dim_max_score_ratio);
        } else if constexpr (algo == InvertedIndexAlgo::DAAT_MAXSCORE) {
            search_daat_maxscore(q_vec, heap, filter, computer, dim_max_score_ratio);
        } else if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_WAND) {
            search_daat_blockmax_wand(q_vec, heap, filter, computer, dim_max_score_ratio);
        } else if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_MAXSCORE) {
            search_daat_blockmax_maxscore(q_vec, heap, filter, computer, dim_max_score_ratio);
        } else {
            search_taat_naive(q_vec, heap, filter, computer);
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
    add_row_to_index(const SparseRow<DType>& row, table_t vec_id, bool is_append) {
        [[maybe_unused]] float row_sum = 0;
        for (size_t j = 0; j < row.size(); ++j) {
            auto [dim, val] = row[j];
            if (metric_type_ == SparseMetricType::METRIC_BM25) {
                row_sum += val;
            }
            // Skip values equals to or close enough to zero(which contributes
            // little to the total IP score).
            if (val == 0) {
                continue;
            }
            auto dim_it = dim_map_.find(dim);
            if (dim_it == dim_map_.cend()) {
                if constexpr (mmapped) {
                    throw std::runtime_error("unexpected vector dimension in mmapped InvertedIndex");
                }
                dim_it = dim_map_.insert({dim, next_dim_id_++}).first;
                inverted_index_ids_.emplace_back();
                inverted_index_vals_.emplace_back();
                if constexpr (algo != InvertedIndexAlgo::TAAT_NAIVE) {
                    max_score_in_dim_.emplace_back(0.0f);
                    if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_WAND ||
                                  algo == InvertedIndexAlgo::DAAT_BLOCKMAX_MAXSCORE) {
                        if (is_append) {
                            blockmax_ids_.emplace_back();
                            blockmax_scores_.emplace_back();
                            blockmax_last_block_sizes_.emplace_back(0);
                        }
                    }
                }
            }
            inverted_index_ids_[dim_it->second].emplace_back(vec_id);
            inverted_index_vals_[dim_it->second].emplace_back(get_quant_val(val));
            if constexpr (algo != InvertedIndexAlgo::TAAT_NAIVE) {
                auto score = static_cast<float>(val);
                if (metric_type_ == SparseMetricType::METRIC_BM25) {
                    score = bm25_params_->max_score_computer(val, row_sum);
                }
                max_score_in_dim_[dim_it->second] = std::max(max_score_in_dim_[dim_it->second], score);
                if constexpr (algo == InvertedIndexAlgo::DAAT_BLOCKMAX_WAND ||
                              algo == InvertedIndexAlgo::DAAT_BLOCKMAX_MAXSCORE) {
                    if (is_append) {
                        size_t cur_block_size = blockmax_last_block_sizes_[dim_it->second];
                        if (cur_block_size == 0) {
                            // create a new block and add the first element
                            blockmax_ids_[dim_it->second].emplace_back(vec_id);
                            blockmax_scores_[dim_it->second].emplace_back(score);
                        } else {
                            // change the element of the last block
                            blockmax_ids_[dim_it->second].back() = vec_id;
                            if (score > blockmax_scores_[dim_it->second].back()) {
                                blockmax_scores_[dim_it->second].back() = score;
                            }
                        }
                        // update the last block size
                        ++cur_block_size;
                        if (cur_block_size >= blockmax_block_size_) {
                            cur_block_size = 0;
                        }
                        blockmax_last_block_sizes_[dim_it->second] = cur_block_size;
                    }
                }
            }
        }
        if (metric_type_ == SparseMetricType::METRIC_BM25) {
            bm25_params_->row_sums.emplace_back(row_sum);
        }
    }

    inline QType
    get_quant_val(DType val) const {
        if constexpr (!std::is_same_v<QType, DType>) {
            const DType max_val = static_cast<DType>(std::numeric_limits<QType>::max());
            if (val >= max_val) {
                return std::numeric_limits<QType>::max();
            } else if (val <= std::numeric_limits<QType>::min()) {
                return std::numeric_limits<QType>::min();
            } else {
                return static_cast<QType>(val);
            }
        } else {
            return val;
        }
    }

    // key is raw sparse vector dim/idx, value is the mapped dim/idx id in the index.
    std::unordered_map<table_t, uint32_t> dim_map_;

    // reserve, [], size, emplace_back
    Vector<Vector<table_t>> inverted_index_ids_;
    Vector<Vector<QType>> inverted_index_vals_;
    Vector<float> max_score_in_dim_;
    Vector<Vector<table_t>> blockmax_ids_;
    Vector<Vector<float>> blockmax_scores_;
    Vector<uint16_t> blockmax_last_block_sizes_;
    size_t blockmax_block_size_ = 0;

    SparseMetricType metric_type_;

    size_t n_rows_internal_ = 0;
    size_t max_dim_ = 0;
    uint32_t next_dim_id_ = 0;

    char* map_ = nullptr;
    size_t map_byte_size_ = 0;
    int map_fd_ = -1;

    char* blockmax_map_ = nullptr;
    size_t blockmax_map_byte_size_ = 0;
    int blockmax_map_fd_ = -1;

    struct BM25Params {
        float k1;
        float b;
        // row_sums is used to cache the sum of values of each row, which
        // corresponds to the document length of each doc in the BM25 formula.
        Vector<float> row_sums;

        DocValueComputer<float> max_score_computer;

        BM25Params(float k1, float b, float avgdl)
            : k1(k1), b(b), max_score_computer(GetDocValueBM25Computer<float>(k1, b, avgdl)) {
        }
    };  // struct BM25Params

    std::unique_ptr<BM25Params> bm25_params_;

};  // class InvertedIndex

}  // namespace knowhere::sparse

#endif  // SPARSE_INVERTED_INDEX_H
