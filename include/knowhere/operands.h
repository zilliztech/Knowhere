// Copyright (C) 2019-2023 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#ifndef OPERANDS_H
#define OPERANDS_H
#include <math.h>

#include <cstdint>
#include <cstring>

#include "feature.h"

namespace {
union fp32_bits {
    uint32_t as_bits;
    float as_value;
};

__attribute__((always_inline)) inline float
bf16_float(float f) {
    auto u32 = fp32_bits{.as_value = f}.as_bits;
    // Round off
    return fp32_bits{.as_bits = (u32 + 0x8000) & 0xFFFF0000}.as_value;
}

inline float
fp32_from_bits(const uint32_t& w) {
    return fp32_bits{.as_bits = w}.as_value;
}

inline uint32_t
fp32_to_bits(const float& f) {
    return fp32_bits{.as_value = f}.as_bits;
}
};  // namespace

namespace knowhere {
using fp32 = float;
using int8 = int8_t;
using bin1 = uint8_t;

struct fp16 {
 public:
    fp16() = default;
    fp16(const float& f) {
        from_fp32(f);
    };
    operator float() const {
        return to_fp32(bits);
    }

 private:
    uint16_t bits = 0;
    void
    from_fp32(const float f) {
        // const float scale_to_inf = 0x1.0p+112f;
        // const float scale_to_zero = 0x1.0p-110f;
        constexpr uint32_t scale_to_inf_bits = (uint32_t)239 << 23;
        constexpr uint32_t scale_to_zero_bits = (uint32_t)17 << 23;
        float scale_to_inf_val, scale_to_zero_val;
        std::memcpy(&scale_to_inf_val, &scale_to_inf_bits, sizeof(scale_to_inf_val));
        std::memcpy(&scale_to_zero_val, &scale_to_zero_bits, sizeof(scale_to_zero_val));
        const float scale_to_inf = scale_to_inf_val;
        const float scale_to_zero = scale_to_zero_val;

#if defined(_MSC_VER) && _MSC_VER == 1916
        float base = ((f < 0.0 ? -f : f) * scale_to_inf) * scale_to_zero;
#else
        float base = (fabsf(f) * scale_to_inf) * scale_to_zero;
#endif

        const uint32_t w = fp32_to_bits(f);
        const uint32_t shl1_w = w + w;
        const uint32_t sign = w & UINT32_C(0x80000000);
        uint32_t bias = shl1_w & UINT32_C(0xFF000000);
        if (bias < UINT32_C(0x71000000)) {
            bias = UINT32_C(0x71000000);
        }

        base = fp32_from_bits((bias >> 1) + UINT32_C(0x07800000)) + base;
        const uint32_t bits = fp32_to_bits(base);
        const uint32_t exp_bits = (bits >> 13) & UINT32_C(0x00007C00);
        const uint32_t mantissa_bits = bits & UINT32_C(0x00000FFF);
        const uint32_t nonsign = exp_bits + mantissa_bits;
        this->bits = static_cast<uint16_t>((sign >> 16) | (shl1_w > UINT32_C(0xFF000000) ? UINT16_C(0x7E00) : nonsign));
    }

    float
    to_fp32(const uint16_t h) const {
        const uint32_t w = (uint32_t)h << 16;
        const uint32_t sign = w & UINT32_C(0x80000000);
        const uint32_t two_w = w + w;

        constexpr uint32_t exp_offset = UINT32_C(0xE0) << 23;
        constexpr uint32_t scale_bits = (uint32_t)15 << 23;

        float exp_scale_val;
        std::memcpy(&exp_scale_val, &scale_bits, sizeof(exp_scale_val));
        const float exp_scale = exp_scale_val;
        const float normalized_value = fp32_from_bits((two_w >> 4) + exp_offset) * exp_scale;

        constexpr uint32_t magic_mask = UINT32_C(126) << 23;
        constexpr float magic_bias = 0.5f;
        const float denormalized_value = fp32_from_bits((two_w >> 17) | magic_mask) - magic_bias;
        constexpr uint32_t denormalized_cutoff = UINT32_C(1) << 27;
        const uint32_t result =
            sign | (two_w < denormalized_cutoff ? fp32_to_bits(denormalized_value) : fp32_to_bits(normalized_value));
        return fp32_from_bits(result);
    }
};

struct bf16 {
 public:
    bf16() = default;
    bf16(const float& f) {
        from_fp32(f);
    };
    operator float() const {
        return this->to_fp32(bits);
    }

 private:
    uint16_t bits = 0;
    void
    from_fp32(const float f) {
        volatile uint32_t fp32Bits = fp32_to_bits(f);
        volatile uint16_t bf16Bits = (uint16_t)(fp32Bits >> 16);
        this->bits = bf16Bits;
    }
    float
    to_fp32(const uint16_t h) const {
        uint32_t bits = ((unsigned int)h) << 16;
        bits &= 0xFFFF0000;
        return fp32_from_bits(bits);
    }
};

template <typename T>
bool
typeCheck(uint64_t features) {
    if constexpr (std::is_same_v<T, bin1>) {
        return features & knowhere::feature::BINARY;
    }
    if constexpr (std::is_same_v<T, fp16>) {
        return features & knowhere::feature::FP16;
    }
    if constexpr (std::is_same_v<T, bf16>) {
        return features & knowhere::feature::BF16;
    }
    // TODO : add sparse_fp32 data type
    if constexpr (std::is_same_v<T, fp32>) {
        return (features & knowhere::feature::FLOAT32) || (features & knowhere::feature::SPARSE_FLOAT32);
    }
    if constexpr (std::is_same_v<T, int8>) {
        return features & knowhere::feature::INT8;
    }
    return false;
}

template <typename InType, typename... Types>
using TypeMatch = std::bool_constant<(... | std::is_same_v<InType, Types>)>;
template <typename InType>
using KnowhereDataTypeCheck = TypeMatch<InType, bin1, fp16, fp32, bf16, int8>;
template <typename InType>
using KnowhereFloatTypeCheck = TypeMatch<InType, fp16, fp32, bf16>;
template <typename InType>
using KnowhereHalfPrecisionFloatPointTypeCheck = TypeMatch<InType, fp16, bf16>;

template <typename T>
struct MockData {
    using type = T;
};

template <>
struct MockData<knowhere::fp16> {
    using type = knowhere::fp32;
};

template <>
struct MockData<knowhere::bf16> {
    using type = knowhere::fp32;
};

template <>
struct MockData<knowhere::int8> {
    using type = knowhere::fp32;
};

//
enum class DataFormatEnum { fp32, fp16, bf16, int8, bin1 };

template <typename T>
struct DataType2EnumHelper {};

template <>
struct DataType2EnumHelper<knowhere::fp32> {
    static constexpr DataFormatEnum value = DataFormatEnum::fp32;
};
template <>
struct DataType2EnumHelper<knowhere::fp16> {
    static constexpr DataFormatEnum value = DataFormatEnum::fp16;
};
template <>
struct DataType2EnumHelper<knowhere::bf16> {
    static constexpr DataFormatEnum value = DataFormatEnum::bf16;
};
template <>
struct DataType2EnumHelper<knowhere::int8> {
    static constexpr DataFormatEnum value = DataFormatEnum::int8;
};
template <>
struct DataType2EnumHelper<knowhere::bin1> {
    static constexpr DataFormatEnum value = DataFormatEnum::bin1;
};

template <typename T>
static constexpr DataFormatEnum datatype_v = DataType2EnumHelper<T>::value;

}  // namespace knowhere
#endif /* OPERANDS_H */
