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

#include <string>
#include <unordered_set>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "knowhere/comp/knowhere_config.h"

TEST_CASE("Knowhere global config", "[init]") {
    knowhere::KnowhereConfig::ShowVersion();

    knowhere::KnowhereConfig::EnablePatchForComputeFP32AsBF16();
    knowhere::KnowhereConfig::DisablePatchForComputeFP32AsBF16();

    int64_t blas_threshold = 16384;
    knowhere::KnowhereConfig::SetBlasThreshold(blas_threshold);
    REQUIRE(knowhere::KnowhereConfig::GetBlasThreshold() == blas_threshold);

    int64_t early_stop_threshold = 0;
    knowhere::KnowhereConfig::SetEarlyStopThreshold(early_stop_threshold);
    REQUIRE(knowhere::KnowhereConfig::GetEarlyStopThreshold() == early_stop_threshold);

    knowhere::KnowhereConfig::SetClusteringType(knowhere::KnowhereConfig::ClusteringType::K_MEANS_PLUS_PLUS);
    knowhere::KnowhereConfig::SetClusteringType(knowhere::KnowhereConfig::ClusteringType::K_MEANS);

    size_t prev_build_thread_num = knowhere::KnowhereConfig::GetBuildThreadPoolSize();
    knowhere::KnowhereConfig::SetBuildThreadPoolSize(8);
    REQUIRE(knowhere::KnowhereConfig::GetBuildThreadPoolSize() == 8);
    if (prev_build_thread_num > 0) {
        knowhere::KnowhereConfig::SetBuildThreadPoolSize(prev_build_thread_num);
    }

    size_t prev_search_thread_num = knowhere::KnowhereConfig::GetSearchThreadPoolSize();
    knowhere::KnowhereConfig::SetSearchThreadPoolSize(8);
    REQUIRE(knowhere::KnowhereConfig::GetSearchThreadPoolSize() == 8);
    if (prev_search_thread_num > 0) {
        knowhere::KnowhereConfig::SetSearchThreadPoolSize(prev_search_thread_num);
    }

#ifdef KNOWHERE_WITH_DISKANN
    REQUIRE_FALSE(knowhere::KnowhereConfig::SetAioContextPool(0));
    REQUIRE(knowhere::KnowhereConfig::SetAioContextPool(16));
#endif

#ifdef KNOWHERE_WITH_CUVS
    knowhere::KnowhereConfig::SetRaftMemPool(1LL << 10, 2LL << 10);
#endif
}

TEST_CASE("Knowhere SIMD config", "[simd]") {
    std::vector<std::string> v = {"AVX512", "AVX2", "SSE4_2", "GENERIC", "NEON"};
    std::unordered_set<std::string> s(v.begin(), v.end());

    auto res = knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::AVX512);
    REQUIRE(s.find(res) != s.end());
    res = knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::AVX2);
    REQUIRE(s.find(res) != s.end());
    res = knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::SSE4_2);
    REQUIRE(s.find(res) != s.end());
    res = knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::GENERIC);
    REQUIRE(s.find(res) != s.end());
    res = knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::AUTO);
    REQUIRE(s.find(res) != s.end());
}
