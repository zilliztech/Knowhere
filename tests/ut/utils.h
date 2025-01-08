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

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <vector>

#include "catch2/generators/catch_generators.hpp"
#include "knowhere/binaryset.h"
#include "knowhere/dataset.h"
#include "knowhere/object.h"
#include "knowhere/range_util.h"
#include "knowhere/version.h"

constexpr int64_t kSeed = 42;
using IdDisPair = std::pair<int64_t, float>;
struct DisPairLess {
    bool
    operator()(const IdDisPair& p1, const IdDisPair& p2) {
        return p1.second < p2.second;
    }
};

inline knowhere::DataSetPtr
GenDataSet(int rows, int dim, int seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<> distrib(0.0, 100.0);
    float* ts = new float[rows * dim];
    for (int i = 0; i < rows * dim; ++i) ts[i] = distrib(rng);
    auto ds = knowhere::GenDataSet(rows, dim, ts);
    ds->SetIsOwner(true);
    return ds;
}

inline knowhere::DataSetPtr
CopyDataSet(knowhere::DataSetPtr dataset, const int64_t copy_rows) {
    REQUIRE(!dataset->GetIsSparse());
    auto rows = copy_rows;
    auto dim = dataset->GetDim();
    auto data = dataset->GetTensor();
    float* ts = new float[rows * dim];
    memcpy(ts, data, rows * dim * sizeof(float));
    auto ds = knowhere::GenDataSet(rows, dim, ts);
    ds->SetIsOwner(true);
    return ds;
}

inline knowhere::DataSetPtr
GenBinDataSet(int rows, int dim, int seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<> distrib(0.0, 100.0);
    int uint8_num = dim / 8;
    uint8_t* ts = new uint8_t[rows * uint8_num];
    for (int i = 0; i < rows * uint8_num; ++i) ts[i] = (uint8_t)distrib(rng);
    auto ds = knowhere::GenDataSet(rows, dim, ts);
    ds->SetIsOwner(true);
    return ds;
}

inline knowhere::DataSetPtr
CopyBinDataSet(knowhere::DataSetPtr dataset, const int64_t copy_rows) {
    REQUIRE(!dataset->GetIsSparse());
    auto rows = copy_rows;
    auto dim = dataset->GetDim();
    auto data = dataset->GetTensor();
    int uint8_num = dim / 8;
    uint8_t* ts = new uint8_t[rows * uint8_num];
    memcpy(ts, data, rows * uint8_num * sizeof(uint8_t));
    auto ds = knowhere::GenDataSet(rows, dim, ts);
    ds->SetIsOwner(true);
    return ds;
}

inline knowhere::DataSetPtr
GenIdsDataSet(int rows, int nq, int64_t seed = 42) {
    std::mt19937 g(seed);
    int64_t* ids = new int64_t[rows];
    for (int i = 0; i < rows; ++i) ids[i] = i;
    std::shuffle(ids, ids + rows, g);
    auto ds = knowhere::GenIdsDataSet(nq, ids);
    ds->SetIsOwner(true);
    return ds;
}

inline knowhere::DataSetPtr
GenIdsDataSet(int rows, std::vector<int64_t>& ids) {
    auto ds = knowhere::GenIdsDataSet(rows, ids.data());
    ds->SetIsOwner(false);
    return ds;
}

inline float
GetKNNRecall(const knowhere::DataSet& ground_truth, const knowhere::DataSet& result) {
    REQUIRE(ground_truth.GetDim() >= result.GetDim());

    auto nq = result.GetRows();
    auto gt_k = ground_truth.GetDim();
    auto res_k = result.GetDim();
    auto gt_ids = ground_truth.GetIds();
    auto res_ids = result.GetIds();

    uint32_t matched_num = 0;
    for (auto i = 0; i < nq; ++i) {
        std::vector<int64_t> ids_0(gt_ids + i * gt_k, gt_ids + i * gt_k + res_k);
        std::vector<int64_t> ids_1(res_ids + i * res_k, res_ids + i * res_k + res_k);

        std::sort(ids_0.begin(), ids_0.end());
        std::sort(ids_1.begin(), ids_1.end());

        std::vector<int64_t> v(std::max(ids_0.size(), ids_1.size()));
        std::vector<int64_t>::iterator it;
        it = std::set_intersection(ids_0.begin(), ids_0.end(), ids_1.begin(), ids_1.end(), v.begin());
        v.resize(it - v.begin());
        matched_num += v.size();
    }
    return ((float)matched_num) / ((float)nq * res_k);
}

inline float
GetKNNRecall(const knowhere::DataSet& ground_truth, const std::vector<std::vector<int64_t>>& result) {
    auto nq = result.size();
    auto gt_k = ground_truth.GetDim();
    auto gt_ids = ground_truth.GetIds();

    uint32_t matched_num = 0;
    for (size_t i = 0; i < nq; ++i) {
        std::vector<int64_t> ids_0(gt_ids + i * gt_k, gt_ids + i * gt_k + gt_k);
        std::vector<int64_t> ids_1 = result[i];

        std::sort(ids_0.begin(), ids_0.end());
        std::sort(ids_1.begin(), ids_1.end());

        std::vector<int64_t> v(std::max(ids_0.size(), ids_1.size()));
        std::vector<int64_t>::iterator it;
        it = std::set_intersection(ids_0.begin(), ids_0.end(), ids_1.begin(), ids_1.end(), v.begin());
        v.resize(it - v.begin());
        matched_num += v.size();
    }
    return ((float)matched_num) / ((float)nq * gt_k);
}

//  Compare two ann-search results
//      "ground_truth" here is just used as a baseline value for comparison. It is not real groundtruth and the knn
//  results may be worse, we can call the compare results as "relative-recall".
//      when the k-th distance of gt is worth, define the recall as 1.0f
//      when the k-th distance of gt is better, define the recall as (intersection_count / size)
inline float
GetKNNRelativeRecall(const knowhere::DataSet& ground_truth, const knowhere::DataSet& result, bool dist_less_better) {
    REQUIRE(ground_truth.GetDim() >= result.GetDim());

    auto nq = result.GetRows();
    auto gt_k = ground_truth.GetDim();
    auto res_k = result.GetDim();
    auto gt_ids = ground_truth.GetIds();
    auto res_ids = result.GetIds();
    auto gt_dists = ground_truth.GetDistance();
    auto res_dists = result.GetDistance();

    double acc_recall = 0;
    for (auto i = 0; i < nq; ++i) {
        // Results may be insufficient, less than k
        int64_t valid_gt_count = 0;
        while (valid_gt_count < gt_k && gt_ids[i * gt_k + valid_gt_count] >= 0) {
            valid_gt_count++;
        }

        if (valid_gt_count == 0) {
            acc_recall += 1.0;
            continue;
        }

        bool gt_better = dist_less_better
                             ? gt_dists[i * gt_k + valid_gt_count - 1] < res_dists[i * res_k + valid_gt_count - 1]
                             : gt_dists[i * gt_k + valid_gt_count - 1] > res_dists[i * res_k + valid_gt_count - 1];

        if (gt_better) {
            std::vector<int64_t> ids_0(gt_ids + i * gt_k, gt_ids + i * gt_k + valid_gt_count);
            std::vector<int64_t> ids_1(res_ids + i * res_k, res_ids + i * res_k + valid_gt_count);

            std::sort(ids_0.begin(), ids_0.end());
            std::sort(ids_1.begin(), ids_1.end());

            std::vector<int64_t> v(std::max(ids_0.size(), ids_1.size()));
            std::vector<int64_t>::iterator it;
            it = std::set_intersection(ids_0.begin(), ids_0.end(), ids_1.begin(), ids_1.end(), v.begin());
            v.resize(it - v.begin());

            acc_recall += double(v.size()) / valid_gt_count;
        } else {
            acc_recall += 1.0;
        }
    }
    return acc_recall / nq;
}

inline float
GetRangeSearchRecall(const knowhere::DataSet& gt, const knowhere::DataSet& result) {
    uint32_t nq = result.GetRows();
    auto res_ids_p = result.GetIds();
    auto res_lims_p = result.GetLims();
    auto gt_ids_p = gt.GetIds();
    auto gt_lims_p = gt.GetLims();

    // check if both gt and result are empty
    if (gt_lims_p[nq] == 0 && res_lims_p[nq] == 0) {
        return 1;
    }

    uint32_t ninter = 0;
    for (uint32_t i = 0; i < nq; ++i) {
        std::set<int64_t> inter;
        std::set<int64_t> res_ids_set(res_ids_p + res_lims_p[i], res_ids_p + res_lims_p[i + 1]);
        std::set<int64_t> gt_ids_set(gt_ids_p + gt_lims_p[i], gt_ids_p + gt_lims_p[i + 1]);
        std::set_intersection(res_ids_set.begin(), res_ids_set.end(), gt_ids_set.begin(), gt_ids_set.end(),
                              std::inserter(inter, inter.begin()));
        ninter += inter.size();
    }

    float recall = ninter * 1.0f / gt_lims_p[nq];
    float precision = ninter * 1.0f / res_lims_p[nq];

    return (1 + precision) * recall / 2;
}

inline float
GetRelativeLoss(float gt_res, float res) {
    if (gt_res == 0.0 || std::abs(gt_res) < 0.000001) {
        return gt_res;
    }
    return std::abs((gt_res - res) / gt_res);
}

inline bool
CheckDistanceInScope(const knowhere::DataSet& result, int topk, float low_bound, float high_bound) {
    auto ids = result.GetIds();
    auto distances = result.GetDistance();
    auto rows = result.GetRows();
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < topk; j++) {
            auto idx = i * topk + j;
            auto id = ids[idx];
            auto d = distances[idx];
            if (id != -1 && !(low_bound < d && d < high_bound)) {
                return false;
            }
        }
    }
    return true;
}

inline bool
CheckDistanceInScope(const knowhere::DataSet& result, float low_bound, float high_bound) {
    auto ids = result.GetIds();
    auto distances = result.GetDistance();
    auto lims = result.GetLims();
    auto rows = result.GetRows();
    for (int i = 0; i < rows; ++i) {
        for (size_t j = lims[i]; j < lims[i + 1]; j++) {
            auto id = ids[j];
            auto d = distances[j];
            if (id != -1 && !(low_bound < d && d < high_bound)) {
                return false;
            }
        }
    }
    return true;
}

inline std::unordered_map<int64_t, std::vector<std::vector<uint32_t>>>
GenerateScalarInfo(size_t n) {
    std::vector<std::vector<uint32_t>> scalar_info;
    scalar_info.reserve(2);
    std::vector<uint32_t> scalar1;
    scalar1.reserve(n / 2);
    std::vector<uint32_t> scalar2;
    scalar2.reserve(n - n / 2);
    for (size_t i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            scalar2.emplace_back(i);
        } else {
            scalar1.emplace_back(i);
        }
    }
    scalar_info.emplace_back(std::move(scalar1));
    scalar_info.emplace_back(std::move(scalar2));
    std::unordered_map<int64_t, std::vector<std::vector<uint32_t>>> scalar_map;
    scalar_map[0] = std::move(scalar_info);
    return scalar_map;
}

inline std::vector<uint8_t>
GenerateBitsetByScalarInfoAndFirstTBits(const std::vector<uint32_t>& scalar, size_t n, size_t t) {
    assert(scalar.size() <= n);
    assert(t >= 0 && t <= n - scalar.size());
    std::vector<uint8_t> data((n + 8 - 1) / 8, 0);
    // set bits by scalar info
    for (size_t i = 0; i < scalar.size(); ++i) {
        data[scalar[i] >> 3] |= (0x1 << (scalar[i] & 0x7));
    }
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (count == t) {
            break;
        }
        // already set, skip
        if (data[i >> 3] & (0x1 << (i & 0x7))) {
            continue;
        }
        data[i >> 3] |= (0x1 << (i & 0x7));
        ++count;
    }
    return data;
}

// Return a n-bits bitset data with first t bits set to true
inline std::vector<uint8_t>
GenerateBitsetWithFirstTbitsSet(size_t n, size_t t) {
    assert(t >= 0 && t <= n);
    std::vector<uint8_t> data((n + 8 - 1) / 8, 0);
    for (size_t i = 0; i < t; ++i) {
        data[i >> 3] |= (0x1 << (i & 0x7));
    }
    return data;
}

// Return a n-bits bitset data with random t bits set to true
inline std::vector<uint8_t>
GenerateBitsetWithRandomTbitsSet(size_t n, size_t t) {
    assert(t >= 0 && t <= n);
    std::vector<bool> bits_shuffle(n, false);
    for (size_t i = 0; i < t; ++i) bits_shuffle[i] = true;
    std::mt19937 g(kSeed);
    std::shuffle(bits_shuffle.begin(), bits_shuffle.end(), g);
    std::vector<uint8_t> data((n + 8 - 1) / 8, 0);
    for (size_t i = 0; i < n; ++i) {
        if (bits_shuffle[i]) {
            data[i >> 3] |= (0x1 << (i & 0x7));
        }
    }
    return data;
}

// Randomly generate n (distances, id) pairs
inline std::vector<std::pair<float, size_t>>
GenerateRandomDistanceIdPair(size_t n) {
    std::mt19937 rng(kSeed);
    std::uniform_real_distribution<> distrib(std::numeric_limits<float>().min(), std::numeric_limits<float>().max());
    std::vector<std::pair<float, size_t>> res;
    res.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        res.emplace_back(distrib(rng), i);
    }
    return res;
}

inline auto
GenTestVersionList() {
    return GENERATE(as<int32_t>{}, knowhere::Version::GetCurrentVersion().VersionNumber());
}

inline knowhere::DataSetPtr
GenSparseDataSet(const std::vector<std::map<int32_t, float>>& data, int32_t cols) {
    int32_t rows = data.size();
    auto tensor = std::make_unique<knowhere::sparse::SparseRow<float>[]>(rows);

    for (int32_t i = 0; i < rows; ++i) {
        if (data[i].size() == 0) {
            continue;
        }
        knowhere::sparse::SparseRow<float> row(data[i].size());
        size_t j = 0;
        for (auto& [idx, val] : data[i]) {
            row.set_at(j++, idx, val);
        }
        tensor[i] = std::move(row);
    }

    auto ds = knowhere::GenDataSet(rows, cols, tensor.release());
    ds->SetIsOwner(true);
    ds->SetIsSparse(true);
    return ds;
}

// Generate a sparse dataset with given sparsity.
inline knowhere::DataSetPtr
GenSparseDataSet(int32_t rows, int32_t cols, float sparsity, int seed = 42) {
    int32_t num_elements = static_cast<int32_t>(rows * cols * (1.0f - sparsity));

    std::mt19937 rng(seed);
    auto real_distrib = std::uniform_real_distribution<float>(0, 1);
    auto row_distrib = std::uniform_int_distribution<int32_t>(0, rows - 1);
    auto col_distrib = std::uniform_int_distribution<int32_t>(0, cols - 1);

    std::vector<std::map<int32_t, float>> data(rows);

    for (int32_t i = 0; i < num_elements; ++i) {
        auto row = row_distrib(rng);
        while (data[row].size() == (size_t)cols) {
            row = row_distrib(rng);
        }
        auto col = col_distrib(rng);
        while (data[row].find(col) != data[row].end()) {
            col = col_distrib(rng);
        }
        auto val = real_distrib(rng);
        data[row][col] = val;
    }

    return GenSparseDataSet(data, cols);
}

// Generate a sparse dataset with given sparsity and max value.
inline knowhere::DataSetPtr
GenSparseDataSetWithMaxVal(int32_t rows, int32_t cols, float sparsity, float max_val, bool use_bm25 = false,
                           int seed = 42) {
    int32_t num_elements = static_cast<int32_t>(rows * cols * (1.0f - sparsity));

    std::mt19937 rng(seed);
    auto real_distrib = std::uniform_real_distribution<float>(0, max_val);
    auto row_distrib = std::uniform_int_distribution<int32_t>(0, rows - 1);
    auto col_distrib = std::uniform_int_distribution<int32_t>(0, cols - 1);

    std::vector<std::map<int32_t, float>> data(rows);

    for (int32_t i = 0; i < num_elements; ++i) {
        auto row = row_distrib(rng);
        while (data[row].size() == (size_t)cols) {
            row = row_distrib(rng);
        }
        auto col = col_distrib(rng);
        while (data[row].find(col) != data[row].end()) {
            col = col_distrib(rng);
        }
        auto val = use_bm25 ? static_cast<float>(static_cast<int32_t>(real_distrib(rng))) : real_distrib(rng);
        data[row][col] = val;
    }

    return GenSparseDataSet(data, cols);
}

// a timer
struct StopWatch {
    using timepoint_t = std::chrono::time_point<std::chrono::steady_clock>;

    timepoint_t Start;

    //
    StopWatch() {
        Start = std::chrono::steady_clock::now();
    }

    //
    double
    elapsed() const {
        const auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - Start;
        return elapsed.count();
    }
};
