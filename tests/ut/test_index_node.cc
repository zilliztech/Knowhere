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
#include "faiss/IndexFlat.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/index/index_node_data_mock_wrapper.h"
#include "knowhere/index/index_node_thread_pool_wrapper.h"
#include "utils.h"

using namespace knowhere;

constexpr const char* INDEX_BASE_FLAT = "BASE_FLAT";

template <typename DataType>
class BaseFlatIndexNode : public IndexNode {
 public:
    BaseFlatIndexNode(const int32_t& /*version*/, const Object& object) {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode constructor";
    }

    Status
    Train(const DataSetPtr dataset, std::shared_ptr<Config> cfg) override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::Train()";
        return Status::success;
    }

    Status
    Add(const DataSetPtr dataset, std::shared_ptr<Config> cfg) override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::Add()";
        return Status::success;
    }

    virtual expected<DataSetPtr>
    Search(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::Search()";
        return expected<DataSetPtr>::Err(Status::not_implemented, "BaseFlatIndexNode::Search() not implemented");
    }

    virtual expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset) const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::RangeSearch()";
        return expected<DataSetPtr>::Err(Status::not_implemented, "BaseFlatIndexNode::RangeSearch() not implemented");
    }

    expected<std::vector<IndexNode::IteratorPtr>>
    AnnIterator(const DataSetPtr dataset, std::unique_ptr<Config> cfg, const BitsetView& bitset,
                bool use_knowhere_search_pool) const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::AnnIterator()";
        return expected<std::vector<IndexNode::IteratorPtr>>::Err(Status::not_implemented,
                                                                  "BaseFlatIndexNode::AnnIterator() not implemented");
    }

    virtual expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::GetVectorByIds()";
        return expected<DataSetPtr>::Err(Status::not_implemented,
                                         "BaseFlatIndexNode::GetVectorByIds() not implemented");
    }

    virtual bool
    HasRawData(const std::string& metric_type) const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::HasRawData()";
        return true;
    }

    expected<DataSetPtr>
    GetIndexMeta(std::unique_ptr<Config> cfg) const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::GetIndexMeta()";
        return expected<DataSetPtr>::Err(Status::not_implemented, "BaseFlatIndexNode::GetIndexMeta() not implemented");
    }

    virtual Status
    Serialize(BinarySet& binset) const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::Serialize()";
        return Status::success;
    }

    virtual Status
    Deserialize(BinarySet&& binset, std::shared_ptr<Config> config) override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::Deserialize()";
        return Status::success;
    }

    virtual Status
    DeserializeFromFile(const std::string& filename, std::shared_ptr<Config> config) override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::DeserializeFromFile()";
        return Status::success;
    }

    static std::unique_ptr<BaseConfig>
    StaticCreateConfig() {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::StaticCreateConfig()";
        return std::make_unique<BaseConfig>();
    }

    virtual std::unique_ptr<BaseConfig>
    CreateConfig() const override {
        LOG_KNOWHERE_INFO_ << "BaseFlatIndexNode::CreateConfig()";
        return std::make_unique<BaseConfig>();
    }

    int64_t
    Dim() const override {
        return 0;
    }

    int64_t
    Size() const override {
        return 0;
    }

    int64_t
    Count() const override {
        return 0;
    }

    std::string
    Type() const override {
        return INDEX_BASE_FLAT;
    }
};

TEST_CASE("Test index node") {
    auto version = GenTestVersionList();
    DataSetPtr ds = std::make_shared<DataSet>();
    BinarySet binset;

    SECTION("Test IndexNode") {
        KNOWHERE_SIMPLE_REGISTER_GLOBAL(BASE_FLAT, BaseFlatIndexNode, fp32, knowhere::feature::FLOAT32);
        auto index = IndexFactory::Instance().Create<fp32>("BASE_FLAT", version).value();
        REQUIRE(index.Build(ds, {}) == Status::success);
        REQUIRE(index.Train(ds, {}) == Status::success);
        REQUIRE(index.Add(ds, {}) == Status::success);
        REQUIRE(index.Search(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.RangeSearch(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.AnnIterator(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.GetVectorByIds(ds).error() == Status::not_implemented);
        REQUIRE(index.HasRawData(metric::L2) == true);
        REQUIRE(index.GetIndexMeta({}).error() == Status::not_implemented);
        REQUIRE(index.Serialize(binset) == Status::success);
        REQUIRE(index.Deserialize(std::move(binset), {}) == Status::success);
        REQUIRE(index.DeserializeFromFile("", {}) == Status::success);
        REQUIRE(index.Dim() == 0);
        REQUIRE(index.Size() == 0);
        REQUIRE(index.Count() == 0);
        REQUIRE(index.Type() == INDEX_BASE_FLAT);
    }

    SECTION("Test IndexNodeDataMockWrapper") {
        KNOWHERE_MOCK_REGISTER_GLOBAL(BASE_FLAT_MOCK, BaseFlatIndexNode, fp16, knowhere::feature::FP16);
        auto index = IndexFactory::Instance().Create<fp16>("BASE_FLAT_MOCK", version).value();
        REQUIRE(index.Build(ds, {}) == Status::success);
        REQUIRE(index.Train(ds, {}) == Status::success);
        REQUIRE(index.Add(ds, {}) == Status::success);
        REQUIRE(index.Search(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.RangeSearch(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.AnnIterator(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.GetVectorByIds(ds).error() == Status::not_implemented);
        REQUIRE(index.HasRawData(metric::L2) == true);
        REQUIRE(index.GetIndexMeta({}).error() == Status::not_implemented);
        REQUIRE(index.Serialize(binset) == Status::success);
        REQUIRE(index.Deserialize(std::move(binset), {}) == Status::success);
        REQUIRE(index.DeserializeFromFile("", {}) == Status::success);
        REQUIRE(index.Dim() == 0);
        REQUIRE(index.Size() == 0);
        REQUIRE(index.Count() == 0);
        REQUIRE(index.Type() == INDEX_BASE_FLAT);
    }

    SECTION("Test IndexNodeThreadPoolWrapper") {
        KNOWHERE_REGISTER_GLOBAL_WITH_THREAD_POOL(BASE_FLAT_THREAD, BaseFlatIndexNode, fp32, knowhere::feature::FLOAT32,
                                                  4);
        auto index = IndexFactory::Instance().Create<fp32>("BASE_FLAT_THREAD", version).value();
        REQUIRE(index.Build(ds, {}) == Status::success);
        REQUIRE(index.Train(ds, {}) == Status::success);
        REQUIRE(index.Add(ds, {}) == Status::success);
        REQUIRE(index.Search(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.RangeSearch(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.AnnIterator(ds, {}, nullptr).error() == Status::not_implemented);
        REQUIRE(index.GetVectorByIds(ds).error() == Status::not_implemented);
        REQUIRE(index.HasRawData(metric::L2) == true);
        REQUIRE(index.GetIndexMeta({}).error() == Status::not_implemented);
        REQUIRE(index.Serialize(binset) == Status::success);
        REQUIRE(index.Deserialize(std::move(binset), {}) == Status::success);
        REQUIRE(index.DeserializeFromFile("", {}) == Status::success);
        REQUIRE(index.Dim() == 0);
        REQUIRE(index.Size() == 0);
        REQUIRE(index.Count() == 0);
        REQUIRE(index.Type() == INDEX_BASE_FLAT);
    }
}
