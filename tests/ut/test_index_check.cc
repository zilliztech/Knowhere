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

#include <unordered_set>

#include "catch2/catch_approx.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "knowhere/comp/index_param.h"
#include "knowhere/comp/knowhere_check.h"
#include "knowhere/index/index_factory.h"

using namespace knowhere;

// knowhere/index/index_table.h
TEST_CASE("Test index and data type check", "[IndexCheckTest]") {
    SECTION("Test valid") {
        // binary index
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, VecType::VECTOR_BINARY));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, VecType::VECTOR_BINARY));

        // faiss index
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IDMAP, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IDMAP, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IDMAP, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IDMAP, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT_CC, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT_CC, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT_CC, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT_CC, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFPQ, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFPQ, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFPQ, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFPQ, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_SCANN, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_SCANN, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_SCANN, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_SCANN, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ8, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ8, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ8, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ8, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ_CC, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ_CC, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ_CC, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ_CC, VecType::VECTOR_INT8));

        // gpu index
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_BRUTEFORCE, VecType::VECTOR_FLOAT));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_BRUTEFORCE, VecType::VECTOR_FLOAT16));
        CHECK_FALSE(
            KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_BRUTEFORCE, VecType::VECTOR_BFLOAT16));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_BRUTEFORCE, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFFLAT, VecType::VECTOR_FLOAT));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFFLAT, VecType::VECTOR_FLOAT16));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFFLAT, VecType::VECTOR_BFLOAT16));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFFLAT, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFPQ, VecType::VECTOR_FLOAT));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFPQ, VecType::VECTOR_FLOAT16));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFPQ, VecType::VECTOR_BFLOAT16));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_IVFPQ, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_CAGRA, VecType::VECTOR_FLOAT));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_CAGRA, VecType::VECTOR_FLOAT16));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_CAGRA, VecType::VECTOR_BFLOAT16));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_GPU_CAGRA, VecType::VECTOR_INT8));

        // HNSW
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_INT8));

#ifdef KNOWHERE_WITH_CARDINAL
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_BINARY));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_SPARSE_FLOAT));
#else
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_BINARY));
        CHECK_FALSE(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW, VecType::VECTOR_SPARSE_FLOAT));
#endif

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_SQ, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_SQ, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_SQ, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_SQ, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PQ, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PQ, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PQ, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PQ, VecType::VECTOR_INT8));

        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PRQ, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PRQ, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PRQ, VecType::VECTOR_BFLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_HNSW_PRQ, VecType::VECTOR_INT8));

        // diskann
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_DISKANN, VecType::VECTOR_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_DISKANN, VecType::VECTOR_FLOAT16));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_DISKANN, VecType::VECTOR_BFLOAT16));

        // sparse index
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX,
                                                       VecType::VECTOR_SPARSE_FLOAT));
        CHECK(KnowhereCheck::IndexTypeAndDataTypeCheck(IndexEnum::INDEX_SPARSE_WAND, VecType::VECTOR_SPARSE_FLOAT));
    }
}

TEST_CASE("Test support mmap index", "[IndexCheckTest]") {
    SECTION("Test valid") {
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT));

        // faiss index
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_IDMAP));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_IVFFLAT));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_IVFPQ));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_SCANN));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ8));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_FAISS_IVFSQ_CC));

        // hnsw
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_HNSW));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_HNSW_SQ));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_HNSW_PQ));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_HNSW_PRQ));

        // sparse index
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX));
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_SPARSE_WAND));

#ifdef KNOWHERE_WITH_CARDINAL
        CHECK(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_DISKANN));
#else
        CHECK_FALSE(KnowhereCheck::SupportMmapIndexTypeCheck(IndexEnum::INDEX_DISKANN));
#endif
    }
}

TEST_CASE("Test index has raw data", "[IndexHasRawData]") {
    SECTION("Normal test") {
        auto ver = Version::GetCurrentVersion().VersionNumber();

        // binary index
        CHECK(knowhere::IndexStaticFaced<bin1>::HasRawData(IndexEnum::INDEX_FAISS_BIN_IDMAP, ver, {}));
        CHECK(knowhere::IndexStaticFaced<bin1>::HasRawData(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, ver, {}));

        // faiss index
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_IDMAP, ver, {}));
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_IVFFLAT, ver, {}));
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_IVFFLAT_CC, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_IVFPQ, ver, {}));
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_SCANN, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_IVFSQ8, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_IVFSQ_CC, ver, {}));

        // HNSW
#ifdef KNOWHERE_WITH_CARDINAL
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW, ver, {}));
#else
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW, ver, {}));
#endif
        // faiss HNSW
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW_PQ, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW_PRQ, ver, {}));

        // diskann
#ifdef KNOWHERE_WITH_DISKANN
#ifndef KNOWHERE_WITH_CARDINAL
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_DISKANN, ver,
                                                           knowhere::Json::parse(R"({"metric_type": "L2"})")));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_DISKANN, ver,
                                                                 knowhere::Json::parse(R"({"metric_type": "IP"})")));
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_DISKANN, ver,
                                                           knowhere::Json::parse(R"({"metric_type": "COSINE"})")));
#endif
#endif
        // gpu index

#ifdef KNOWHERE_WITH_CUVS
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_RAFT_BRUTEFORCE, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_RAFT_IVFFLAT, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_RAFT_IVFPQ, ver, {}));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_RAFT_CAGRA, ver, {}));
#endif
        // sparse index
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_SPARSE_INVERTED_INDEX, ver, {}));
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_SPARSE_WAND, ver, {}));
    }

    SECTION("Special test") {
        auto min_ver = Version::GetMinimalVersion().VersionNumber();
        auto ver = Version::GetCurrentVersion().VersionNumber();

        knowhere::Json json = {
            {indexparam::WITH_RAW_DATA, true},
        };

        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(
            IndexEnum::INDEX_FAISS_IDMAP, min_ver, knowhere::Json::parse(R"({"metric_type": "COSINE"})")));
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_SCANN, ver, json));

        json[indexparam::WITH_RAW_DATA] = false;
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_SCANN, ver, json));
        json[indexparam::WITH_RAW_DATA] = "true";
        CHECK(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_SCANN, ver, json));
        json[indexparam::WITH_RAW_DATA] = "false";
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_SCANN, ver, json));
        json[indexparam::WITH_RAW_DATA] = 1;
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_FAISS_SCANN, ver, json));

        knowhere::Json faiss_hnsw_cfg = {{"refine", true}, {"refine_type", "bf16"}};
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK(knowhere::IndexStaticFaced<bf16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));

        faiss_hnsw_cfg["refine_type"] = "fp16";
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK_FALSE(knowhere::IndexStaticFaced<bf16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK(knowhere::IndexStaticFaced<fp16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));

        faiss_hnsw_cfg["refine_type"] = "sq8";
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK_FALSE(knowhere::IndexStaticFaced<bf16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));

        faiss_hnsw_cfg["refine_type"] = "sq6";
        CHECK_FALSE(knowhere::IndexStaticFaced<fp32>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK_FALSE(knowhere::IndexStaticFaced<bf16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
        CHECK_FALSE(knowhere::IndexStaticFaced<fp16>::HasRawData(IndexEnum::INDEX_HNSW_SQ, ver, faiss_hnsw_cfg));
    }
}

TEST_CASE("Test index feature check", "[IndexFeatureCheck]") {
    SECTION("Check MMap") {
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::MMAP));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT_CC, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFSQ8, knowhere::feature::MMAP));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFSQ_CC, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::MMAP));

        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::MMAP));

#ifdef KNOWHERE_WITH_CUVS
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::MMAP));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::MMAP));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::MMAP));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::MMAP));
#endif
#ifdef KNOWHERE_WITH_DISKANN
#ifdef KNOWHERE_WITH_CARDINAL
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::DISK));
#else
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::MMAP));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::DISK));
#endif
#endif

        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::DISK));
    }

    SECTION("Check GPU") {
#ifdef KNOWHERE_WITH_CUVS
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::GPU));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::GPU));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::GPU));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::GPU));
#endif

        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::GPU));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::GPU));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::GPU));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::GPU));

#ifdef KNOWHERE_WITH_DISKANN
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::GPU));
#endif
    }

    SECTION("Check DataType") {
        // FAISS Flat Indexes
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::SPARSE_FLOAT32));

        // FAISS IVF Indexes
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::SPARSE_FLOAT32));

        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::SPARSE_FLOAT32));

        // FAISS Binary Indexes
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::FP16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::BF16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::SPARSE_FLOAT32));

        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, knowhere::feature::FP16));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, knowhere::feature::BF16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, knowhere::feature::BINARY));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT,
                                                            knowhere::feature::SPARSE_FLOAT32));

        // HNSW Index
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::BF16));
#ifndef KNOWHERE_WITH_CARDINAL
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::INT8));
#endif

#ifdef KNOWHERE_WITH_CARDINAL
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::BINARY));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::SPARSE_FLOAT32));
#else
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::SPARSE_FLOAT32));
#endif

        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_SQ, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_SQ, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_SQ, knowhere::feature::BF16));
#ifndef KNOWHERE_WITH_CARDINAL
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_SQ, knowhere::feature::INT8));
#endif
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PQ, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PQ, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PQ, knowhere::feature::BF16));
#ifndef KNOWHERE_WITH_CARDINAL
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PQ, knowhere::feature::INT8));
#endif
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PRQ, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PRQ, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PRQ, knowhere::feature::BF16));
#ifndef KNOWHERE_WITH_CARDINAL
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PRQ, knowhere::feature::INT8));
#endif
        // Sparse Indexes
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX, knowhere::feature::FP16));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX, knowhere::feature::BF16));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX, knowhere::feature::BINARY));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX,
                                                      knowhere::feature::SPARSE_FLOAT32));

        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_WAND, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_WAND, knowhere::feature::FP16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_WAND, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_WAND, knowhere::feature::BINARY));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_WAND, knowhere::feature::SPARSE_FLOAT32));

        // GPU Indexes
#ifdef KNOWHERE_WITH_CUVS
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::FP16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::BF16));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::SPARSE_FLOAT32));

        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::FP16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::SPARSE_FLOAT32));

        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::FP16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::SPARSE_FLOAT32));

        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::FLOAT32));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::FP16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::SPARSE_FLOAT32));
#endif

        // DiskANN Index
#ifdef KNOWHERE_WITH_DISKANN
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::SPARSE_FLOAT32));

#endif

        // SCANN Index
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::FLOAT32));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::FP16));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::BF16));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::BINARY));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::SPARSE_FLOAT32));
    }

    SECTION("Check NoTrain") {
        // Flat indexes typically don't require training
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::NO_TRAIN));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::NO_TRAIN));

        // Indexes that typically require training
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, knowhere::feature::NO_TRAIN));

        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_WAND, knowhere::feature::NO_TRAIN));

#ifdef KNOWHERE_WITH_CUVS
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::NO_TRAIN));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::NO_TRAIN));
#endif

#ifdef KNOWHERE_WITH_DISKANN
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::NO_TRAIN));
#endif
    }

    SECTION("Check MV") {
        // HNSW family supports Materialized View
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW, knowhere::feature::MV));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_SQ, knowhere::feature::MV));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_SQ, knowhere::feature::MV));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_SQ, knowhere::feature::MV));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PQ, knowhere::feature::MV));
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_HNSW_PRQ, knowhere::feature::MV));

#ifdef KNOWHERE_WITH_DISKANN
#ifdef KNOWHERE_WITH_CARDINAL
        // cardinal diskann supports mv
        REQUIRE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::MV));
#else
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_DISKANN, knowhere::feature::MV));
#endif
#endif
        // All other indexes do not support MV
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IDMAP, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFFLAT, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFPQ, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_IVFSQ8, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_SCANN, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IDMAP, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_FAISS_BIN_IVFFLAT, knowhere::feature::MV));

        REQUIRE_FALSE(
            IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_INVERTED_INDEX, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_SPARSE_WAND, knowhere::feature::MV));

#ifdef KNOWHERE_WITH_CUVS
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_BRUTEFORCE, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFFLAT, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_IVFPQ, knowhere::feature::MV));
        REQUIRE_FALSE(IndexFactory::Instance().FeatureCheck(IndexEnum::INDEX_RAFT_CAGRA, knowhere::feature::MV));
#endif
    }
}
