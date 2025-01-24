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

#ifndef INDEX_H
#define INDEX_H

#include "knowhere/binaryset.h"
#include "knowhere/config.h"
#include "knowhere/dataset.h"
#include "knowhere/expected.h"
#include "knowhere/index/index_node.h"
#include "knowhere/index/interrupt.h"
namespace knowhere {

template <typename T1>
class Index {
 public:
    template <typename T2>
    friend class Index;

    Index() : node(nullptr) {
    }

    template <typename... Args>
    static Index<T1>
    Create(Args&&... args) {
        return Index(new (std::nothrow) T1(std::forward<Args>(args)...));
    }

    Index(const Index<T1>& idx) {
        if (idx.node == nullptr) {
            node = nullptr;
            return;
        }
        idx.node->IncRef();
        node = idx.node;
    }

    Index<T1>&
    operator=(const Index<T1>& idx) {
        if (&idx == this) {
            return *this;
        }
        if (idx.node == nullptr) {
            node = nullptr;
            return *this;
        }
        idx.node->IncRef();
        node = idx.node;
        return *this;
    }

    Index(Index<T1>&& idx) {
        if (idx.node == nullptr) {
            node = nullptr;
            return;
        }
        node = idx.node;
        idx.node = nullptr;
    }

    template <typename T2>
    Index(const Index<T2>& idx) {
        static_assert(std::is_base_of<T1, T2>::value);
        if (idx.node == nullptr) {
            node = nullptr;
            return;
        }
        idx.node->IncRef();
        node = idx.node;
    }

    template <typename T2>
    Index(Index<T2>&& idx) {
        static_assert(std::is_base_of<T1, T2>::value);
        if (idx.node == nullptr) {
            node = nullptr;
            return;
        }
        node = idx.node;
        idx.node = nullptr;
    }

    template <typename T2>
    Index<T1>&
    operator=(const Index<T2>& idx) {
        static_assert(std::is_base_of<T1, T2>::value);
        if (node != nullptr) {
            node->DecRef();
            if (!node->Ref())
                delete node;
        }
        if (idx.node == nullptr) {
            node = nullptr;
            return *this;
        }
        node = idx.node;
        node->IncRef();
        return *this;
    }

    template <typename T2>
    Index<T1>&
    operator=(Index<T2>&& idx) {
        static_assert(std::is_base_of<T1, T2>::value);
        if (node != nullptr) {
            node->DecRef();
            if (!node->Ref())
                delete node;
        }
        node = idx.node;
        idx.node = nullptr;
        return *this;
    }

    T1*
    Node() {
        return node;
    }

    const T1*
    Node() const {
        return node;
    }

    template <typename T2>
    Index<T2>
    Cast() {
        static_assert(std::is_base_of<T1, T2>::value);
        node->IncRef();
        return Index(dynamic_cast<T2>(node));
    }

    Status
    Build(const DataSetPtr dataset, const Json& json);

#ifdef KNOWHERE_WITH_CARDINAL
    const std::shared_ptr<Interrupt>
    BuildAsync(const DataSetPtr dataset, const Json& json,
               const std::chrono::seconds timeout = std::chrono::seconds::max());
#else
    const std::shared_ptr<Interrupt>
    BuildAsync(const DataSetPtr dataset, const Json& json);
#endif

    Status
    Train(const DataSetPtr dataset, const Json& json);

    Status
    Add(const DataSetPtr dataset, const Json& json);

    expected<DataSetPtr>
    Search(const DataSetPtr dataset, const Json& json, const BitsetView& bitset) const;

    expected<std::vector<IndexNode::IteratorPtr>>
    AnnIterator(const DataSetPtr dataset, const Json& json, const BitsetView& bitset,
                bool use_knowhere_search_pool = true) const;

    expected<DataSetPtr>
    RangeSearch(const DataSetPtr dataset, const Json& json, const BitsetView& bitset) const;

    expected<DataSetPtr>
    GetVectorByIds(const DataSetPtr dataset) const;

    bool
    HasRawData(const std::string& metric_type) const;

    bool
    IsAdditionalScalarSupported(bool is_mv_only) const;

    expected<DataSetPtr>
    GetIndexMeta(const Json& json) const;

    Status
    Serialize(BinarySet& binset) const;

    Status
    Deserialize(BinarySet&& binset, const Json& json = {});

    Status
    DeserializeFromFile(const std::string& filename, const Json& json = {});

    int64_t
    Dim() const;

    int64_t
    Size() const;

    int64_t
    Count() const;

    std::string
    Type() const;

    ~Index() {
        if (node == nullptr)
            return;
        node->DecRef();
        if (!node->Ref()) {
            delete node;
            node = nullptr;
        }
    }

 private:
    Index(T1* node) : node(node) {
        static_assert(std::is_base_of<IndexNode, T1>::value);
    }

    T1* node;
};

}  // namespace knowhere

#endif /* INDEX_H */
