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

#include <strings.h>

#include <algorithm>
#include <vector>

#include "knowhere/binaryset.h"
#include "knowhere/dataset.h"
#include "knowhere/operands.h"

namespace knowhere {

extern const float FloatAccuracy;

inline bool
IsMetricType(const std::string& str, const knowhere::MetricType& metric_type) {
    return !strcasecmp(str.data(), metric_type.c_str());
}

inline bool
IsFlatIndex(const knowhere::IndexType& index_type) {
    static std::vector<knowhere::IndexType> flat_index_list = {
        IndexEnum::INDEX_FAISS_IDMAP, IndexEnum::INDEX_FAISS_GPU_IDMAP, IndexEnum::INDEX_GPU_BRUTEFORCE};
    return std::find(flat_index_list.begin(), flat_index_list.end(), index_type) != flat_index_list.end();
}

template <typename DataType>
extern float
NormalizeVec(DataType* x, int32_t d);

template <typename DataType>
extern std::vector<float>
NormalizeVecs(DataType* x, size_t rows, int32_t dim);

template <typename DataType>
extern std::unique_ptr<DataType[]>
CopyAndNormalizeVecs(const DataType* x, size_t rows, int32_t dim);

template <typename DataType>
extern void
NormalizeDataset(const DataSetPtr dataset);

constexpr inline uint64_t seed = 0xc70f6907UL;

inline uint64_t
hash_vec(const float* x, size_t d) {
    uint64_t h = seed;
    for (size_t i = 0; i < d; ++i) {
        h = h * 13331 + *(uint32_t*)(x + i);
    }
    return h;
}

inline uint64_t
hash_u8_vec(const uint8_t* x, size_t d) {
    uint64_t h = seed;
    for (size_t i = 0; i < d; ++i) {
        h = h * 13331 + *(x + i);
    }
    return h;
}

inline uint64_t
hash_binary_vec(const uint8_t* x, size_t d) {
    size_t len = (d + 7) / 8;
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h = h * 13331 + x[i];
    }
    return h;
}

inline uint64_t
hash_half_precision_float(const void* x, size_t d) {
    uint64_t h = seed;
    auto u16_x = (uint16_t*)(x);
    for (size_t i = 0; i < d; ++i) {
        h = h * 13331 + u16_x[i];
    }
    return h;
}

template <typename DataType>
inline std::string
GetKey(const std::string& name) {
    static_assert(KnowhereDataTypeCheck<DataType>::value == true);
    if (std::is_same_v<DataType, fp32>) {
        return name + std::string("_fp32");
    } else if (std::is_same_v<DataType, fp16>) {
        return name + std::string("_fp16");
    } else if (std::is_same_v<DataType, bf16>) {
        return name + std::string("_bf16");
    } else if (std::is_same_v<DataType, bin1>) {
        return name + std::string("_bin1");
    } else if (std::is_same_v<DataType, int8>) {
        return name + std::string("_int8");
    }
}

template <typename InType, typename OutType>
inline DataSetPtr
data_type_conversion(const DataSet& src, const std::optional<int64_t> start = std::nullopt,
                     const std::optional<int64_t> count = std::nullopt) {
    auto dim = src.GetDim();
    auto rows = src.GetRows();

    // check the acceptable range
    int64_t start_row = start.value_or(0);
    if (start_row < 0 || start_row >= rows) {
        return nullptr;
    }

    int64_t count_rows = count.value_or(rows - start_row);
    if (count_rows < 0 || start_row + count_rows > rows) {
        return nullptr;
    }

    // map
    auto* des_data = new OutType[dim * count_rows];
    auto* src_data = (const InType*)src.GetTensor();
    for (auto i = 0; i < dim * count_rows; i++) {
        des_data[i] = (OutType)src_data[i + start_row * dim];
    }

    auto des = std::make_shared<DataSet>();
    des->SetRows(count_rows);
    des->SetDim(dim);
    des->SetTensor(des_data);
    des->SetIsOwner(true);
    return des;
}

// Convert DataSet from DataType to float
// * no start, no count, float -> returns the source without cloning
// * no start, no count, no float -> returns a clone with a different type
// * start, no count -> returns a clone that starts from a given row 'start'
// * no start, count -> returns a clone that starts from a row 0 and has 'count' rows
// * start, count -> returns a clone that start from a given row 'start' and has 'count' rows
// * invalid start, count values -> returns nullptr
template <typename DataType>
inline DataSetPtr
ConvertFromDataTypeIfNeeded(const DataSetPtr& ds, const std::optional<int64_t> start = std::nullopt,
                            const std::optional<int64_t> count = std::nullopt) {
    if constexpr (std::is_same_v<DataType, typename MockData<DataType>::type>) {
        if (!start.has_value() && !count.has_value()) {
            return ds;
        }
    }

    return data_type_conversion<DataType, typename MockData<DataType>::type>(*ds, start, count);
}

// Convert DataSet from float to DataType
template <typename DataType>
inline DataSetPtr
ConvertToDataTypeIfNeeded(const DataSetPtr& ds, const std::optional<int64_t> start = std::nullopt,
                          const std::optional<int64_t> count = std::nullopt) {
    if constexpr (std::is_same_v<DataType, typename MockData<DataType>::type>) {
        if (!start.has_value() && !count.has_value()) {
            return ds;
        }
    }

    return data_type_conversion<typename MockData<DataType>::type, DataType>(*ds, start, count);
}

template <typename T>
inline T
round_down(const T value, const T align) {
    return value / align * align;
}

bool
UseDiskLoad(const std::string& index_type, const int32_t& /*version*/);

template <typename T, typename W>
static void
writeBinaryPOD(W& out, const T& podRef) {
    out.write((char*)&podRef, sizeof(T));
}

template <typename T, typename R>
static void
readBinaryPOD(R& in, T& podRef) {
    in.read((char*)&podRef, sizeof(T));
}

// taken from
// https://github.com/Microsoft/BLAS-on-flash/blob/master/include/utils.h
// round up X to the nearest multiple of Y
#define ROUND_UP(X, Y) ((((uint64_t)(X) / (Y)) + ((uint64_t)(X) % (Y) != 0)) * (Y))

#define DIV_ROUND_UP(X, Y) (((uint64_t)(X) / (Y)) + ((uint64_t)(X) % (Y) != 0))

// round down X to the nearest multiple of Y
#define ROUND_DOWN(X, Y) (((uint64_t)(X) / (Y)) * (Y))

}  // namespace knowhere
