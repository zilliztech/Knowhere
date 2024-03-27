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

#include "knowhere/binaryset.h"
#include "knowhere/bitsetview.h"
#include "knowhere/config.h"
#include "knowhere/dataset.h"
#include "knowhere/expected.h"
#include "knowhere/object.h"
#include "knowhere/operands.h"
#include "knowhere/version.h"

namespace knowhere {
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

    virtual Status
    Build(const DataSet& dataset, const Config& cfg) {
        RETURN_IF_ERROR(Train(dataset, cfg));
        return Add(dataset, cfg);
    }

    virtual Status
    Train(const DataSet& dataset, const Config& cfg) = 0;

    virtual Status
    Add(const DataSet& dataset, const Config& cfg) = 0;

    virtual expected<DataSetPtr>
    Search(const DataSet& dataset, const Config& cfg, const BitsetView& bitset) const = 0;

    // not thread safe.
    class iterator {
     public:
        virtual std::pair<int64_t, float>
        Next() = 0;
        [[nodiscard]] virtual bool
        HasNext() const = 0;
        virtual ~iterator() {
        }
    };

    virtual expected<std::vector<std::shared_ptr<iterator>>>
    AnnIterator(const DataSet& dataset, const Config& cfg, const BitsetView& bitset) const {
        throw std::runtime_error("annIterator not supported for current index type");
    }

    virtual expected<DataSetPtr>
    RangeSearch(const DataSet& dataset, const Config& cfg, const BitsetView& bitset) const = 0;

    virtual expected<DataSetPtr>
    GetVectorByIds(const DataSet& dataset) const = 0;

    virtual bool
    HasRawData(const std::string& metric_type) const = 0;

    virtual bool
    IsAdditionalScalarSupported() const {
        return false;
    }

    virtual expected<DataSetPtr>
    GetIndexMeta(const Config& cfg) const = 0;

    virtual Status
    Serialize(BinarySet& binset) const = 0;

    virtual Status
    Deserialize(const BinarySet& binset, const Config& config) = 0;

    virtual Status
    DeserializeFromFile(const std::string& filename, const Config& config) = 0;

    virtual std::unique_ptr<BaseConfig>
    CreateConfig() const = 0;

    virtual int64_t
    Dim() const = 0;

    virtual int64_t
    Size() const = 0;

    virtual int64_t
    Count() const = 0;

    virtual std::string
    Type() const = 0;

    virtual ~IndexNode() {
    }

 protected:
    Version version_;
};

// An iterator implementation that accepts a list of distances and ids and returns them in order.
class PrecomputedDistanceIterator : public IndexNode::iterator {
 public:
    PrecomputedDistanceIterator(std::vector<std::pair<float, int64_t>>&& distances_ids, bool larger_is_closer)
        : comp_(larger_is_closer), results_(std::move(distances_ids)) {
        sort_size_ = std::max((size_t)50000, results_.size() / 10);
        sort_next();
    }

    // Construct an iterator from a list of distances with index being id, filtering out zero distances.
    PrecomputedDistanceIterator(std::vector<float> distances, bool larger_is_closer) : comp_(larger_is_closer) {
        for (size_t i = 0; i < distances.size(); i++) {
            if (distances[i] != 0) {
                results_.push_back(std::make_pair(distances[i], i));
            }
        }
        sort_size_ = std::max((size_t)50000, results_.size() / 10);
        sort_next();
    }

    std::pair<int64_t, float>
    Next() override {
        sort_next();
        auto& result = results_[next_++];
        return std::make_pair(result.second, result.first);
    }

    [[nodiscard]] bool
    HasNext() const override {
        return next_ < results_.size() && results_[next_].second != -1;
    }

 private:
    // sort the next sort_size_ elements
    inline void
    sort_next() {
        if (next_ < sorted_) {
            return;
        }
        size_t current_end = std::min(results_.size(), sorted_ + sort_size_);
        std::partial_sort(results_.begin() + sorted_, results_.begin() + current_end, results_.end(), comp_);
        sorted_ = current_end;
    }
    struct PairComparator {
        bool larger_is_closer;
        PairComparator(bool larger) : larger_is_closer(larger) {
        }

        bool
        operator()(const std::pair<float, int64_t>& a, const std::pair<float, int64_t>& b) const {
            if (a.second == -1) {
                return false;
            }
            if (b.second == -1) {
                return true;
            }
            return larger_is_closer ? a.first > b.first : a.first < b.first;
        }
    } comp_;

    std::vector<std::pair<float, int64_t>> results_;
    size_t next_ = 0;
    size_t sorted_ = 0;
    size_t sort_size_ = 0;
};

}  // namespace knowhere

#endif /* INDEX_NODE_H */
