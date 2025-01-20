#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <stdexcept>

#include "io/file_io.h"
#include "knowhere/bitsetview.h"
#include "knowhere/operands.h"
#include "knowhere/utils.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma once

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>

#include <atomic>
#include <list>
#include <random>
#include <unordered_set>

#include "hnswlib.h"
#include "io/memory_io.h"
#include "knowhere/config.h"
#include "knowhere/heap.h"
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
#include "knowhere/prometheus_client.h"
#endif
#include "knowhere/utils.h"
#include "neighbor.h"
#include "visited_list_pool.h"

#if defined(__SSE__)
#include <immintrin.h>
#define USE_PREFETCH
#endif

namespace hnswlib {
typedef unsigned int tableint;
typedef unsigned int linklistsizeint;
constexpr float kHnswSearchKnnBFFilterThreshold = 0.93f;
constexpr float kHnswSearchRangeBFFilterThreshold = 0.97f;
constexpr float kHnswSearchBFTopkThreshold = 0.5f;

enum Metric {
    L2 = 0,
    INNER_PRODUCT = 1,
    COSINE = 2,
    HAMMING = 10,
    JACCARD = 11,
    UNKNOWN = 100,
};

enum QuantType { None = 0, SQ8 = 1, SQ8Refine = 2 };

template <typename data_t, typename dist_t, QuantType quant_type>
class HierarchicalNSW : public AlgorithmInterface<dist_t> {
    static_assert(std::is_same_v<data_t, knowhere::bin1> || std::is_same_v<data_t, knowhere::fp32> ||
                  std::is_same_v<data_t, knowhere::fp16> || std::is_same_v<data_t, knowhere::bf16>);

 public:
    bool base_layer_only = {false};
    int num_seeds = 32;
    static const tableint max_update_element_locks = 65536;

    static constexpr bool sq_enabled = quant_type != QuantType::None && knowhere::KnowhereFloatTypeCheck<data_t>::value;
    static constexpr bool has_raw_data = quant_type == QuantType::None || quant_type == QuantType::SQ8Refine;

    HierarchicalNSW(SpaceInterface<dist_t>* s) {
    }

    HierarchicalNSW(SpaceInterface<dist_t>* s, const std::string& location, bool nmslib = false,
                    size_t max_elements = 0) {
        loadIndex(location, s, max_elements);
    }

    HierarchicalNSW(SpaceInterface<dist_t>* s, size_t max_elements, size_t M = 16, size_t ef_construction = 200,
                    size_t random_seed = 100)
        : link_list_locks_(max_elements),
          link_list_update_locks_(max_update_element_locks),
          element_levels_(max_elements) {
        space_ = s;
        if constexpr (knowhere::KnowhereFloatTypeCheck<data_t>::value) {
            if (auto x = dynamic_cast<L2Space<data_t, dist_t>*>(s)) {
                metric_type_ = Metric::L2;
            } else if (auto x = dynamic_cast<InnerProductSpace<data_t, dist_t>*>(s)) {
                metric_type_ = Metric::INNER_PRODUCT;
            } else if (auto x = dynamic_cast<CosineSpace<data_t, dist_t>*>(s)) {
                metric_type_ = Metric::COSINE;
            } else {
                metric_type_ = Metric::UNKNOWN;
            }
        } else {
            if (auto x = dynamic_cast<HammingSpace*>(s)) {
                metric_type_ = Metric::HAMMING;
            } else if (auto x = dynamic_cast<JaccardSpace*>(s)) {
                metric_type_ = Metric::JACCARD;
            } else {
                metric_type_ = Metric::UNKNOWN;
            }
        }

        max_elements_ = max_elements;

        num_deleted_ = 0;
        data_size_ = s->get_data_size();
        fstdistfunc_ = s->get_dist_func();
        if constexpr (sq_enabled) {
            fstdistfunc_sq_ = space_->get_dist_func_sq();
        }
        dist_func_param_ = s->get_dist_func_param();
        M_ = M;
        maxM_ = M_;
        maxM0_ = M_ * 2;
        ef_construction_ = std::max(ef_construction, M_);
        ef_ = 10;

        level_generator_.seed(random_seed);
        update_probability_generator_.seed(random_seed + 1);

        size_links_level0_ = maxM0_ * sizeof(tableint) + sizeof(linklistsizeint);
        size_data_per_element_ = size_links_level0_;  // + sizeof(labeltype);
        if constexpr (has_raw_data) {
            size_data_per_element_ += data_size_;
        }
        if constexpr (sq_enabled) {
            size_data_per_element_ += *(size_t*)dist_func_param_ * sizeof(int8_t);
        }
        offsetData_ = size_links_level0_;
        if constexpr (sq_enabled) {
            offsetSQData_ = offsetData_;
            if constexpr (has_raw_data) {
                offsetSQData_ += data_size_;
            }
        }
        // label_offset_ = size_links_level0_ + data_size_;
        offsetLevel0_ = 0;

        data_level0_memory_ = (char*)malloc(max_elements_ * size_data_per_element_);  // NOLINT
        if (data_level0_memory_ == nullptr)
            throw std::runtime_error("Not enough memory");

        if (metric_type_ == Metric::COSINE) {
            data_norm_l2_ = (float*)malloc(max_elements_ * sizeof(float));  // NOLINT
            if (data_norm_l2_ == nullptr)
                throw std::runtime_error("Not enough memory");
        }

        cur_element_count = 0;

        visited_list_pool_ = new VisitedListPool(max_elements);

        // initializations for special treatment of the first node
        enterpoint_node_ = -1;
        maxlevel_ = -1;

        linkLists_ = (char**)malloc(sizeof(void*) * max_elements_);
        if (linkLists_ == nullptr)
            throw std::runtime_error("Not enough memory: HierarchicalNSW failed to allocate linklists");
        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint);
        mult_ = 1 / log(1.0 * M_);
        revSize_ = 1.0 / mult_;
    }

    struct CompareByFirst {
        constexpr bool
        operator()(std::pair<dist_t, tableint> const& a, std::pair<dist_t, tableint> const& b) const noexcept {
            return a.first < b.first;
        }
    };

    ~HierarchicalNSW() {
        if (mmap_enabled_) {
            munmap(map_, map_size_);
        } else {
            free(data_level0_memory_);
            if (metric_type_ == Metric::COSINE) {
                free(data_norm_l2_);
            }
        }

        for (tableint i = 0; i < cur_element_count; i++) {
            if (element_levels_[i] > 0)
                free(linkLists_[i]);
        }
        free(linkLists_);
        delete visited_list_pool_;

        delete space_;
    }

    // used for free resource
    SpaceInterface<dist_t>* space_;
    size_t metric_type_;  // 0:L2, 1:IP, 2:COSINE

    size_t max_elements_;
    size_t cur_element_count;
    size_t size_data_per_element_;
    size_t size_links_per_element_;
    size_t num_deleted_;

    size_t M_;
    size_t maxM_;
    size_t maxM0_;
    size_t ef_construction_;

    double mult_, revSize_;
    int maxlevel_;

    VisitedListPool* visited_list_pool_;
    std::mutex cur_element_count_guard_;

    std::vector<std::mutex> link_list_locks_;

    // Locks to prevent race condition during update/insert of an element at same time.
    // Note: Locks for additions can also be used to prevent this race condition if the querying of KNN is not exposed
    // along with update/inserts i.e multithread insert/update/query in parallel.
    std::vector<std::mutex> link_list_update_locks_;
    tableint enterpoint_node_;

    size_t size_links_level0_;
    size_t offsetData_, offsetSQData_, offsetLevel0_;

    char* data_level0_memory_;
    float* data_norm_l2_;  // vector's l2 norm
    char** linkLists_;
    std::vector<int> element_levels_;

    size_t data_size_;

    size_t label_offset_;
    DISTFUNC<dist_t> fstdistfunc_;
    DISTFUNC<dist_t> fstdistfunc_sq_;
    void* dist_func_param_;

    std::default_random_engine level_generator_;
    std::default_random_engine update_probability_generator_;

    bool mmap_enabled_{false};
    char* map_;
    size_t map_size_;

    float alpha_ = 0.0f;

    // Symmetric quantization to encode each element value from [-alpha, alpha] to [-127, 127]
    void
    trainSQuant(const data_t* train_data, size_t ntrain) {
        alpha_ = 0.0f;
        size_t dim = *(size_t*)dist_func_param_;
        for (size_t i = 0; i < ntrain; ++i) {
            const data_t* vec = train_data + i * dim;
            std::unique_ptr<data_t[]> vec_norm = nullptr;
            if (metric_type_ == Metric::COSINE) {
                vec_norm = knowhere::CopyAndNormalizeVecs(vec, 1, dim);
                vec = vec_norm.get();
            }
            for (size_t j = 0; j < dim; ++j) {
                alpha_ = std::max(alpha_, std::abs((float)vec[j]));
            }
        }
    }

    void
    encodeSQuant(const data_t* from, int8_t* to) const {
        size_t dim = *(size_t*)dist_func_param_;
        std::unique_ptr<data_t[]> data_norm = nullptr;
        if (metric_type_ == Metric::COSINE) {
            data_norm = knowhere::CopyAndNormalizeVecs(from, 1, dim);
            from = data_norm.get();
        }
        for (size_t i = 0; i < dim; ++i) {
            float x = (float)from[i] / alpha_;
            if (x > 1.0f) {
                x = 1.0f;
            }
            if (x < -1.0f) {
                x = -1.0f;
            }
            to[i] = std::round(x * 127.0f);
        }
    }

    inline char*
    getSQDataByInternalId(tableint internal_id) const {
        return (data_level0_memory_ + internal_id * size_data_per_element_ + offsetSQData_);
    }

    inline char*
    getDataByInternalId(tableint internal_id) const {
        return (data_level0_memory_ + internal_id * size_data_per_element_ + offsetData_);
    }

    int
    getRandomLevel(double reverse_size) {
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        double r = -log(distribution(level_generator_)) * reverse_size;
        return (int)r;
    }

    inline dist_t
    calcDistance(const tableint id1, const tableint id2) const {
        if constexpr (sq_enabled) {
            return fstdistfunc_sq_(getSQDataByInternalId(id1), getSQDataByInternalId(id2), dist_func_param_) * alpha_ *
                   alpha_ / 127.0f / 127.0f;
        } else {
            dist_t dist = fstdistfunc_(getDataByInternalId(id1), getDataByInternalId(id2), dist_func_param_);
            if (metric_type_ == Metric::COSINE) {
                dist /= (data_norm_l2_[id1] * data_norm_l2_[id2]);
            }
            return dist;
        }
    }

    inline dist_t
    calcDistance(const void* vec, const tableint id) const {
        if constexpr (sq_enabled) {
            return fstdistfunc_sq_(vec, getSQDataByInternalId(id), dist_func_param_) * alpha_ * alpha_ / 127.0f /
                   127.0f;
        } else {
            dist_t dist = fstdistfunc_(vec, getDataByInternalId(id), dist_func_param_);
            if (metric_type_ == Metric::COSINE) {
                dist /= data_norm_l2_[id];
            }
            return dist;
        }
    }

    inline dist_t
    calcRefineDistance(const void* vec, const tableint id) const {
        dist_t dist = fstdistfunc_(vec, getDataByInternalId(id), dist_func_param_);
        if (metric_type_ == Metric::COSINE) {
            dist /= data_norm_l2_[id];
        }
        return dist;
    }

    void
    prefetchData(const tableint id) const {
#if defined(USE_PREFETCH)
        if constexpr (sq_enabled) {
            _mm_prefetch(getSQDataByInternalId(id), _MM_HINT_T0);
        } else {
            _mm_prefetch(getDataByInternalId(id), _MM_HINT_T0);
        }
#endif
    }

    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
    searchBaseLayer(tableint ep_id, tableint cur_c, int layer) {
        auto& visited = visited_list_pool_->getFreeVisitedList();

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            top_candidates;
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            candidateSet;

        dist_t lowerBound;
        dist_t dist = calcDistance(cur_c, ep_id);
        top_candidates.emplace(dist, ep_id);
        lowerBound = dist;
        candidateSet.emplace(-dist, ep_id);
        visited[ep_id] = true;

        while (!candidateSet.empty()) {
            std::pair<dist_t, tableint> curr_el_pair = candidateSet.top();
            if ((-curr_el_pair.first) > lowerBound && top_candidates.size() == ef_construction_) {
                break;
            }
            candidateSet.pop();

            tableint curNodeNum = curr_el_pair.second;

            std::unique_lock<std::mutex> lock(link_list_locks_[curNodeNum]);

            int* data;  // = (int *)(linkList0_ + curNodeNum * size_links_per_element0_);
            if (layer == 0) {
                data = (int*)get_linklist0(curNodeNum);
            } else {
                data = (int*)get_linklist(curNodeNum, layer);
                // data = (int *) (linkLists_[curNodeNum] + (layer - 1) * size_links_per_element_);
            }
            size_t size = getListCount((linklistsizeint*)data);
            tableint* datal = (tableint*)(data + 1);
            for (size_t j = 0; j < size; ++j) {
                prefetchData(datal[j]);
            }
            for (size_t j = 0; j < size; j++) {
                tableint candidate_id = *(datal + j);
                // if (candidate_id == 0) continue;
                if (visited[candidate_id]) {
                    continue;
                }
                visited[candidate_id] = true;

                dist_t dist1 = calcDistance(cur_c, candidate_id);
                if (top_candidates.size() < ef_construction_ || lowerBound > dist1) {
                    candidateSet.emplace(-dist1, candidate_id);
                    prefetchData(candidateSet.top().second);

                    top_candidates.emplace(dist1, candidate_id);

                    if (top_candidates.size() > ef_construction_)
                        top_candidates.pop();

                    if (!top_candidates.empty())
                        lowerBound = top_candidates.top().first;
                }
            }
        }

        return top_candidates;
    }

    mutable std::atomic<long> metric_distance_computations;
    mutable std::atomic<long> metric_hops;

    template <typename AddSearchCandidate, bool has_deletions, bool collect_metrics = false>
    inline void
    searchBaseLayerSTNext(const void* data_point, Neighbor next, std::vector<bool>& visited, float& accumulative_alpha,
                          const knowhere::BitsetView& bitset, AddSearchCandidate& add_search_candidate,
                          const knowhere::feder::hnsw::FederResultUniq& feder_result = nullptr) const {
        auto [u, d, s] = next;
        tableint* list = (tableint*)get_linklist0(u);
        int size = list[0];

        if constexpr (collect_metrics) {
            metric_hops++;
            metric_distance_computations += size;
        }
        float kAlpha = bitset.filter_ratio() * 0.7f;
        for (size_t i = 1; i <= size; ++i) {
            if (i + 1 <= size) {
                prefetchData(list[i + 1]);
            }
            tableint v = list[i];
            if (visited[v]) {
                if (feder_result != nullptr) {
                    feder_result->visit_info_.AddVisitRecord(0, u, v, -1.0);
                    feder_result->id_set_.insert(u);
                    feder_result->id_set_.insert(v);
                }
                continue;
            }
            visited[v] = true;
            int status = Neighbor::kValid;
            if (has_deletions && bitset.test((int64_t)v)) {
                status = Neighbor::kInvalid;

                accumulative_alpha += kAlpha;
                if (accumulative_alpha < 1.0f) {
                    continue;
                }
                accumulative_alpha -= 1.0f;
            }
            dist_t dist = calcDistance(data_point, v);
            if (feder_result != nullptr) {
                feder_result->visit_info_.AddVisitRecord(0, u, v, dist);
                feder_result->id_set_.insert(u);
                feder_result->id_set_.insert(v);
            }

            Neighbor nn(v, dist, status);
            if (add_search_candidate(nn)) {
#if defined(USE_PREFETCH)
                _mm_prefetch(get_linklist0(v), _MM_HINT_T0);
#endif
            }
        }
    }

    // accumulative_alpha: when searching on graph with filter, we want to keep some filtered nodes in the search path
    // to not destroy the connectivity of the graph; but we do not want to keep all of them as they won't be candidates.
    // Thus we include only a subset of filtered nodes(controlled by kAlpha) in the search path.
    template <bool has_deletions, bool collect_metrics = false>
    NeighborSetDoublePopList
    searchBaseLayerST(tableint ep_id, const void* data_point, size_t ef, std::vector<bool>& visited,
                      const knowhere::BitsetView& bitset,
                      const knowhere::feder::hnsw::FederResultUniq& feder_result = nullptr,
                      IteratorMinHeap* disqualified = nullptr, float accumulative_alpha = 1.0f) const {
        if (feder_result != nullptr) {
            feder_result->visit_info_.AddLevelVisitRecord(0);
        }
        NeighborSetDoublePopList retset(ef);

        dist_t dist = calcDistance(data_point, ep_id);
        if (!has_deletions || !bitset.test((int64_t)ep_id)) {
            retset.insert(Neighbor(ep_id, dist, Neighbor::kValid));
        } else {
            retset.insert(Neighbor(ep_id, dist, Neighbor::kInvalid));
        }

        visited[ep_id] = true;
        auto add_search_candidate = [&](Neighbor n) { return retset.insert(n, disqualified); };
        size_t hops = 0;
        while (retset.has_next()) {
            searchBaseLayerSTNext<decltype(add_search_candidate), has_deletions, collect_metrics>(
                data_point, retset.pop(), visited, accumulative_alpha, bitset, add_search_candidate, feder_result);
            hops++;
        }
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
        knowhere::knowhere_hnsw_search_hops.Observe(hops);
#endif
        return retset;
    }

    std::vector<tableint>
    getNeighborsByHeuristic2(std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>,
                                                 CompareByFirst>& top_candidates,
                             const size_t M) {
        std::vector<tableint> return_list;

        if (top_candidates.size() < M) {
            return_list.resize(top_candidates.size());
            for (int i = static_cast<int>(top_candidates.size() - 1); i >= 0; i--) {
                return_list[i] = top_candidates.top().second;
                top_candidates.pop();
            }
        } else if (M > 0) {
            return_list.reserve(M);
            std::vector<std::pair<dist_t, tableint>> queue_closest;
            queue_closest.resize(top_candidates.size());
            for (int i = static_cast<int>(top_candidates.size() - 1); i >= 0; i--) {
                queue_closest[i] = top_candidates.top();
                top_candidates.pop();
            }

            for (std::pair<dist_t, tableint>& current_pair : queue_closest) {
                bool good = true;
                for (tableint id : return_list) {
                    dist_t curdist = calcDistance(id, current_pair.second);
                    if (curdist < current_pair.first) {
                        good = false;
                        break;
                    }
                }
                if (good) {
                    return_list.push_back(current_pair.second);
                    if (return_list.size() >= M) {
                        break;
                    }
                }
            }
        }

        return return_list;
    }

    std::vector<std::pair<dist_t, labeltype>>
    getNeighboursWithinRadius(NeighborSetDoublePopList& top_candidates, const void* data_point, float radius,
                              const knowhere::BitsetView& bitset) const {
        std::vector<std::pair<dist_t, labeltype>> result;
        auto& visited = visited_list_pool_->getFreeVisitedList();

        std::queue<std::pair<dist_t, tableint>> radius_queue;
        int i = top_candidates.size() - 1;
        while (i >= 0) {
            auto cand = top_candidates[i--];
            if (cand.distance < radius) {
                radius_queue.push({cand.distance, cand.id});
                result.emplace_back(cand.distance, cand.id);
            }
            visited[cand.id] = true;
        }

        while (!radius_queue.empty()) {
            auto cur = radius_queue.front();
            radius_queue.pop();

            tableint current_id = cur.second;
            int* data = (int*)get_linklist0(current_id);
            size_t size = getListCount((linklistsizeint*)data);

            for (size_t j = 1; j <= size; ++j) {
                prefetchData(data[j]);
            }
            for (size_t j = 1; j <= size; j++) {
                int candidate_id = *(data + j);
                if (!visited[candidate_id]) {
                    visited[candidate_id] = true;
                    if (bitset.empty() || !bitset.test((int64_t)candidate_id)) {
                        dist_t dist = calcDistance(data_point, candidate_id);
                        if (dist < radius) {
                            radius_queue.push({dist, candidate_id});
                            result.emplace_back(dist, candidate_id);
                        }
                    }
                }
            }
        }

        return result;
    }

    linklistsizeint*
    get_linklist0(tableint internal_id) const {
        return (linklistsizeint*)(data_level0_memory_ + internal_id * size_data_per_element_ + offsetLevel0_);
    };

    linklistsizeint*
    get_linklist0(tableint internal_id, char* data_level0_memory_) const {
        return (linklistsizeint*)(data_level0_memory_ + internal_id * size_data_per_element_ + offsetLevel0_);
    };

    linklistsizeint*
    get_linklist(tableint internal_id, int level) const {
        return (linklistsizeint*)(linkLists_[internal_id] + (level - 1) * size_links_per_element_);
    };

    linklistsizeint*
    get_linklist_at_level(tableint internal_id, int level) const {
        return level == 0 ? get_linklist0(internal_id) : get_linklist(internal_id, level);
    };

    tableint
    mutuallyConnectNewElement(const void* data_point, tableint cur_c,
                              std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>,
                                                  CompareByFirst>& top_candidates,
                              int level, bool isUpdate) {
        size_t Mcurmax = level ? maxM_ : maxM0_;

        std::vector<tableint> selectedNeighbors(getNeighborsByHeuristic2(top_candidates, M_));
        if (selectedNeighbors.size() > M_)
            throw std::runtime_error("Should be not be more than M_ candidates returned by the heuristic");

        tableint next_closest_entry_point = selectedNeighbors.front();
        {
            linklistsizeint* ll_cur;
            if (level == 0)
                ll_cur = get_linklist0(cur_c);
            else
                ll_cur = get_linklist(cur_c, level);

            if (*ll_cur && !isUpdate) {
                throw std::runtime_error("The newly inserted element should have blank link list");
            }
            setListCount(ll_cur, selectedNeighbors.size());
            tableint* data = (tableint*)(ll_cur + 1);
            for (size_t idx = 0; idx < selectedNeighbors.size(); idx++) {
                if (data[idx] && !isUpdate)
                    throw std::runtime_error("Possible memory corruption");
                if (level > element_levels_[selectedNeighbors[idx]])
                    throw std::runtime_error("Trying to make a link on a non-existent level");

                data[idx] = selectedNeighbors[idx];
            }
        }

        for (size_t idx = 0; idx < selectedNeighbors.size(); idx++) {
            std::unique_lock<std::mutex> lock(link_list_locks_[selectedNeighbors[idx]]);

            linklistsizeint* ll_other;
            if (level == 0)
                ll_other = get_linklist0(selectedNeighbors[idx]);
            else
                ll_other = get_linklist(selectedNeighbors[idx], level);

            size_t sz_link_list_other = getListCount(ll_other);

            if (sz_link_list_other > Mcurmax)
                throw std::runtime_error("Bad value of sz_link_list_other");
            if (selectedNeighbors[idx] == cur_c)
                throw std::runtime_error("Trying to connect an element to itself");
            if (level > element_levels_[selectedNeighbors[idx]])
                throw std::runtime_error("Trying to make a link on a non-existent level");

            tableint* data = (tableint*)(ll_other + 1);

            bool is_cur_c_present = false;
            if (isUpdate) {
                for (size_t j = 0; j < sz_link_list_other; j++) {
                    if (data[j] == cur_c) {
                        is_cur_c_present = true;
                        break;
                    }
                }
            }

            // If cur_c is already present in the neighboring connections of `selectedNeighbors[idx]` then no need to
            // modify any connections or run the heuristics.
            if (!is_cur_c_present) {
                if (sz_link_list_other < Mcurmax) {
                    data[sz_link_list_other] = cur_c;
                    setListCount(ll_other, sz_link_list_other + 1);
                } else {
                    // finding the "weakest" element to replace it with the new one
                    dist_t d_max = calcDistance(cur_c, selectedNeighbors[idx]);
                    // Heuristic:
                    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>,
                                        CompareByFirst>
                        candidates;
                    candidates.emplace(d_max, cur_c);

                    for (size_t j = 0; j < sz_link_list_other; j++) {
                        dist_t dist = calcDistance(data[j], selectedNeighbors[idx]);
                        candidates.emplace(dist, data[j]);
                    }

                    std::vector<tableint> selected(getNeighborsByHeuristic2(candidates, Mcurmax));
                    setListCount(ll_other, static_cast<unsigned short int>(selected.size()));
                    for (size_t i = 0; i < selected.size(); i++) {
                        data[i] = selected[i];
                    }
                    // Nearest K:
                    /*int indx = -1;
                    for (int j = 0; j < sz_link_list_other; j++) {
                        dist_t d = fstdistfunc_(getDataByInternalId(data[j]), getDataByInternalId(rez[idx]),
                    dist_func_param_); if (d > d_max) { indx = j; d_max = d;
                        }
                    }
                    if (indx >= 0) {
                        data[indx] = cur_c;
                    } */
                }
            }
        }

        return next_closest_entry_point;
    }

    std::mutex global;
    size_t ef_;

    // Do not call this to set EF in multi-thread case. This is not thread-safe.
    void
    setEf(size_t ef) {
        ef_ = ef;
    }

    void
    resizeIndex(size_t new_max_elements) {
        if (new_max_elements < cur_element_count)
            throw std::runtime_error("Cannot resize, max element is less than the current number of elements");

        delete visited_list_pool_;
        visited_list_pool_ = new VisitedListPool(new_max_elements);

        element_levels_.resize(new_max_elements);

        std::vector<std::mutex>(new_max_elements).swap(link_list_locks_);

        // Reallocate base layer
        char* data_level0_memory_new = (char*)realloc(data_level0_memory_, new_max_elements * size_data_per_element_);
        if (data_level0_memory_new == nullptr)
            throw std::runtime_error("Not enough memory: resizeIndex failed to allocate base layer");
        data_level0_memory_ = data_level0_memory_new;

        // for COSINE, resize data_norm_l2_
        if (metric_type_ == Metric::COSINE) {
            float* data_norm_l2_new = (float*)realloc(data_norm_l2_, new_max_elements * sizeof(float));
            if (data_norm_l2_new == nullptr)
                throw std::runtime_error("Not enough memory: resizeIndex failed to allocate base layer");
            data_norm_l2_ = data_norm_l2_new;
        }

        // Reallocate all other layers
        char** linkLists_new = (char**)realloc(linkLists_, sizeof(void*) * new_max_elements);
        if (linkLists_new == nullptr)
            throw std::runtime_error("Not enough memory: resizeIndex failed to allocate other layers");
        linkLists_ = linkLists_new;

        max_elements_ = new_max_elements;
    }

    void
    loadIndex(const std::string& location, const knowhere::Config& config, size_t max_elements_i = 0) {
        using knowhere::readBinaryPOD;
        auto cfg = static_cast<const knowhere::BaseConfig&>(config);

        auto input = knowhere::FileReader(location);
        map_size_ = input.size();
        int map_flags = MAP_SHARED;
        if (cfg.enable_mmap_pop.has_value() && cfg.enable_mmap_pop.value()) {
#ifdef MAP_POPULATE
            map_flags |= MAP_POPULATE;
#endif
        }
        map_ = static_cast<char*>(mmap(nullptr, map_size_, PROT_READ, map_flags, input.descriptor(), 0));
        madvise(map_, map_size_, MADV_RANDOM);

        size_t dim;
        readBinaryPOD(input, metric_type_);
        readBinaryPOD(input, data_size_);
        readBinaryPOD(input, dim);
        if constexpr (knowhere::KnowhereFloatTypeCheck<data_t>::value) {
            if (metric_type_ == Metric::L2) {
                space_ = new hnswlib::L2Space<data_t, dist_t>(dim);
            } else if (metric_type_ == Metric::INNER_PRODUCT) {
                space_ = new hnswlib::InnerProductSpace<data_t, dist_t>(dim);
            } else if (metric_type_ == Metric::COSINE) {
                space_ = new hnswlib::CosineSpace<data_t, dist_t>(dim);
            } else {
                throw std::runtime_error("Invalid metric type for float data type(float32, float16 and bfloat16):" +
                                         std::to_string(metric_type_));
            }
        } else {
            if (metric_type_ == Metric::HAMMING) {
                space_ = new hnswlib::HammingSpace(dim);
            } else if (metric_type_ == Metric::JACCARD) {
                space_ = new hnswlib::JaccardSpace(dim);
            } else {
                throw std::runtime_error("Invalid metric type for binary data type :" + std::to_string(metric_type_));
            }
        }

        fstdistfunc_ = space_->get_dist_func();
        dist_func_param_ = space_->get_dist_func_param();
        if constexpr (sq_enabled) {
            readBinaryPOD(input, alpha_);
            fstdistfunc_sq_ = space_->get_dist_func_sq();
        }

        readBinaryPOD(input, offsetLevel0_);
        readBinaryPOD(input, max_elements_);
        readBinaryPOD(input, cur_element_count);

        size_t max_elements = max_elements_i;
        if (max_elements < cur_element_count) {
            max_elements = max_elements_;
        }
        max_elements_ = max_elements;
        readBinaryPOD(input, size_data_per_element_);
        readBinaryPOD(input, label_offset_);
        readBinaryPOD(input, offsetData_);
        if constexpr (sq_enabled) {
            offsetSQData_ = offsetData_;
            if constexpr (has_raw_data) {
                offsetSQData_ += data_size_;
            }
        }
        readBinaryPOD(input, maxlevel_);
        readBinaryPOD(input, enterpoint_node_);

        readBinaryPOD(input, maxM_);
        readBinaryPOD(input, maxM0_);
        readBinaryPOD(input, M_);
        readBinaryPOD(input, mult_);
        readBinaryPOD(input, ef_construction_);

        if (cfg.enable_mmap.has_value() && cfg.enable_mmap.value()) {
            mmap_enabled_ = true;
            // For HNSW, we only mmap the data part, but not the linked lists,
            // which affects the performance significantly
            data_level0_memory_ = map_ + input.offset();
            input.advance(cur_element_count * size_data_per_element_);

            // for COSINE, need load data_norm_l2_
            if (metric_type_ == Metric::COSINE) {
                data_norm_l2_ = reinterpret_cast<float*>(map_ + input.offset());
                input.advance(cur_element_count * sizeof(float));
            }
        } else {
            data_level0_memory_ = (char*)malloc(max_elements * size_data_per_element_);  // NOLINT
            input.read(data_level0_memory_, cur_element_count * size_data_per_element_);

            // for COSINE, need load data_norm_l2_
            if (metric_type_ == Metric::COSINE) {
                data_norm_l2_ = (float*)malloc(max_elements * sizeof(float));  // NOLINT
                input.read((char*)data_norm_l2_, cur_element_count * sizeof(float));
            }
        }

        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint);

        size_links_level0_ = maxM0_ * sizeof(tableint) + sizeof(linklistsizeint);

        visited_list_pool_ = new VisitedListPool(max_elements);

        linkLists_ = (char**)malloc(sizeof(void*) * max_elements);  // NOLINT
        if (linkLists_ == nullptr) {
            throw std::runtime_error("Not enough memory: loadIndex failed to allocate linklists");
        }
        element_levels_ = std::vector<int>(max_elements);
        revSize_ = 1.0 / mult_;
        ef_ = 10;
        for (size_t i = 0; i < cur_element_count; i++) {
            unsigned int linkListSize;
            readBinaryPOD(input, linkListSize);
            if (linkListSize == 0) {
                element_levels_[i] = 0;
                linkLists_[i] = nullptr;
            } else {
                element_levels_[i] = linkListSize / size_links_per_element_;
                linkLists_[i] = (char*)malloc(linkListSize);
                if (linkLists_[i] == nullptr) {
                    throw std::runtime_error("Not enough memory: loadIndex failed to allocate linklist");
                }
                input.read(linkLists_[i], linkListSize);
            }
        }
    }

    void
    saveIndex(knowhere::MemoryIOWriter& output) {
        using knowhere::writeBinaryPOD;
        // write l2/ip calculator
        writeBinaryPOD(output, metric_type_);
        writeBinaryPOD(output, data_size_);
        writeBinaryPOD(output, *((size_t*)dist_func_param_));
        if constexpr (sq_enabled) {
            writeBinaryPOD(output, alpha_);
        }

        writeBinaryPOD(output, offsetLevel0_);
        writeBinaryPOD(output, max_elements_);
        writeBinaryPOD(output, cur_element_count);
        writeBinaryPOD(output, size_data_per_element_);
        writeBinaryPOD(output, label_offset_);
        writeBinaryPOD(output, offsetData_);
        writeBinaryPOD(output, maxlevel_);
        writeBinaryPOD(output, enterpoint_node_);
        writeBinaryPOD(output, maxM_);

        writeBinaryPOD(output, maxM0_);
        writeBinaryPOD(output, M_);
        writeBinaryPOD(output, mult_);
        writeBinaryPOD(output, ef_construction_);

        output.write(data_level0_memory_, cur_element_count * size_data_per_element_);
        // for COSINE, need save data_norm_l2_
        if (metric_type_ == Metric::COSINE) {
            output.write(data_norm_l2_, cur_element_count * sizeof(float));
        }

        for (size_t i = 0; i < cur_element_count; i++) {
            unsigned int linkListSize = element_levels_[i] > 0 ? size_links_per_element_ * element_levels_[i] : 0;
            writeBinaryPOD(output, linkListSize);
            if (linkListSize)
                output.write(linkLists_[i], linkListSize);
        }

        // output.close();
    }

    void
    loadIndex(knowhere::ZeroCopyIOReader& input, size_t max_elements_i = 0) {
        using knowhere::readBinaryPOD;
        // linxj: init with metrictype
        size_t dim;
        readBinaryPOD(input, metric_type_);
        readBinaryPOD(input, data_size_);
        readBinaryPOD(input, dim);
        if constexpr (knowhere::KnowhereFloatTypeCheck<data_t>::value) {
            if (metric_type_ == Metric::L2) {
                space_ = new hnswlib::L2Space<data_t, dist_t>(dim);
            } else if (metric_type_ == Metric::INNER_PRODUCT) {
                space_ = new hnswlib::InnerProductSpace<data_t, dist_t>(dim);
            } else if (metric_type_ == Metric::COSINE) {
                space_ = new hnswlib::CosineSpace<data_t, dist_t>(dim);
            } else {
                throw std::runtime_error("Invalid metric type of float type(float32, float16 and bfloat16):" +
                                         std::to_string(metric_type_));
            }
        } else {
            if (metric_type_ == Metric::HAMMING) {
                space_ = new hnswlib::HammingSpace(dim);
            } else if (metric_type_ == Metric::JACCARD) {
                space_ = new hnswlib::JaccardSpace(dim);
            } else {
                throw std::runtime_error("Invalid metric type of binary type:" + std::to_string(metric_type_));
            }
        }
        fstdistfunc_ = space_->get_dist_func();
        dist_func_param_ = space_->get_dist_func_param();
        if constexpr (sq_enabled) {
            readBinaryPOD(input, alpha_);
            fstdistfunc_sq_ = space_->get_dist_func_sq();
        }

        readBinaryPOD(input, offsetLevel0_);
        readBinaryPOD(input, max_elements_);
        readBinaryPOD(input, cur_element_count);

        size_t max_elements = max_elements_i;
        if (max_elements < cur_element_count) {
            max_elements = max_elements_;
        }
        max_elements_ = max_elements;
        readBinaryPOD(input, size_data_per_element_);
        readBinaryPOD(input, label_offset_);
        readBinaryPOD(input, offsetData_);
        if constexpr (sq_enabled) {
            offsetSQData_ = offsetData_;
            if constexpr (has_raw_data) {
                offsetSQData_ += data_size_;
            }
        }
        readBinaryPOD(input, maxlevel_);
        readBinaryPOD(input, enterpoint_node_);

        readBinaryPOD(input, maxM_);
        readBinaryPOD(input, maxM0_);
        readBinaryPOD(input, M_);
        readBinaryPOD(input, mult_);
        readBinaryPOD(input, ef_construction_);


        input.getDataView(reinterpret_cast<void **>(&data_level0_memory_), size_data_per_element_, cur_element_count);

        // for COSINE, need load data_norm_l2_
        if (metric_type_ == Metric::COSINE) {
            input.getDataView(reinterpret_cast<void **>(data_norm_l2_), sizeof(float), cur_element_count);
        }

        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint);

        size_links_level0_ = maxM0_ * sizeof(tableint) + sizeof(linklistsizeint);

        visited_list_pool_ = new VisitedListPool(max_elements);

        element_levels_ = std::vector<int>(max_elements);
        revSize_ = 1.0 / mult_;
        ef_ = 10;
        for (size_t i = 0; i < cur_element_count; i++) {
            unsigned int linkListSize;
            readBinaryPOD(input, linkListSize);
            if (linkListSize == 0) {
                element_levels_[i] = 0;
                linkLists_[i] = nullptr;
            } else {
                element_levels_[i] = linkListSize / size_links_per_element_;
                input.getDataView(reinterpret_cast<void **>(&linkLists_[i]), sizeof(char), linkListSize);
            }
        }
    }

    unsigned short int
    getListCount(linklistsizeint* ptr) const {
        return *((unsigned short int*)ptr);
    }

    void
    setListCount(linklistsizeint* ptr, unsigned short int size) const {
        *((unsigned short int*)(ptr)) = *((unsigned short int*)&size);
    }

    void
    addPoint(const void* data_point, labeltype label) {
        addPoint(data_point, label, -1);
    }

    void
    updatePoint(const void* dataPoint, tableint internalId, float updateNeighborProbability) {
        // update the feature vector associated with existing point with new vector
        memcpy(getDataByInternalId(internalId), dataPoint, data_size_);

        int maxLevelCopy = maxlevel_;
        tableint entryPointCopy = enterpoint_node_;
        // If point to be updated is entry point and graph just contains single element then just return.
        if (entryPointCopy == internalId && cur_element_count == 1)
            return;

        int elemLevel = element_levels_[internalId];
        std::uniform_real_distribution<float> distribution(0.0, 1.0);
        for (int layer = 0; layer <= elemLevel; layer++) {
            std::unordered_set<tableint> sCand;
            std::unordered_set<tableint> sNeigh;
            std::vector<tableint> listOneHop = getConnectionsWithLock(internalId, layer);
            if (listOneHop.size() == 0)
                continue;

            sCand.insert(internalId);

            for (auto&& elOneHop : listOneHop) {
                sCand.insert(elOneHop);

                if (distribution(update_probability_generator_) > updateNeighborProbability)
                    continue;

                sNeigh.insert(elOneHop);

                std::vector<tableint> listTwoHop = getConnectionsWithLock(elOneHop, layer);
                for (auto&& elTwoHop : listTwoHop) {
                    sCand.insert(elTwoHop);
                }
            }

            for (auto&& neigh : sNeigh) {
                // if (neigh == internalId)
                //     continue;

                std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>,
                                    CompareByFirst>
                    candidates;
                size_t size = sCand.find(neigh) == sCand.end()
                                  ? sCand.size()
                                  : sCand.size() - 1;  // sCand guaranteed to have size >= 1
                size_t elementsToKeep = std::min(ef_construction_, size);
                for (auto&& cand : sCand) {
                    if (cand == neigh)
                        continue;

                    dist_t distance = calcDistance(neigh, cand);
                    if (candidates.size() < elementsToKeep) {
                        candidates.emplace(distance, cand);
                    } else {
                        if (distance < candidates.top().first) {
                            candidates.pop();
                            candidates.emplace(distance, cand);
                        }
                    }
                }

                // Retrieve neighbours using heuristic and set connections.
                getNeighborsByHeuristic2(candidates, layer == 0 ? maxM0_ : maxM_);

                {
                    std::unique_lock<std::mutex> lock(link_list_locks_[neigh]);
                    linklistsizeint* ll_cur;
                    ll_cur = get_linklist_at_level(neigh, layer);
                    size_t candSize = candidates.size();
                    setListCount(ll_cur, candSize);
                    tableint* data = (tableint*)(ll_cur + 1);
                    for (size_t idx = 0; idx < candSize; idx++) {
                        data[idx] = candidates.top().second;
                        candidates.pop();
                    }
                }
            }
        }

        repairConnectionsForUpdate(dataPoint, entryPointCopy, internalId, elemLevel, maxLevelCopy);
    };

    void
    repairConnectionsForUpdate(const void* dataPoint, tableint entryPointInternalId, tableint dataPointInternalId,
                               int dataPointLevel, int maxLevel) {
        tableint currObj = entryPointInternalId;
        if (dataPointLevel < maxLevel) {
            dist_t curdist = calcDistance(dataPoint, currObj);
            for (int level = maxLevel; level > dataPointLevel; level--) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    unsigned int* data;
                    std::unique_lock<std::mutex> lock(link_list_locks_[currObj]);
                    data = get_linklist_at_level(currObj, level);
                    int size = getListCount(data);
                    tableint* datal = (tableint*)(data + 1);
                    for (int i = 0; i < size; ++i) {
                        prefetchData(datal[i]);
                    }
                    for (int i = 0; i < size; i++) {
                        tableint cand = datal[i];
                        dist_t d = calcDistance(dataPoint, cand);
                        if (d < curdist) {
                            curdist = d;
                            currObj = cand;
                            changed = true;
                        }
                    }
                }
            }
        }

        if (dataPointLevel > maxLevel)
            throw std::runtime_error("Level of item to be updated cannot be bigger than max level");

        for (int level = dataPointLevel; level >= 0; level--) {
            std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
                topCandidates = searchBaseLayer(currObj, dataPoint, level);

            std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
                filteredTopCandidates;
            while (topCandidates.size() > 0) {
                if (topCandidates.top().second != dataPointInternalId)
                    filteredTopCandidates.push(topCandidates.top());

                topCandidates.pop();
            }

            // Since element_levels_ is being used to get `dataPointLevel`, there could be cases where `topCandidates`
            // could just contains entry point itself. To prevent self loops, the `topCandidates` is filtered and thus
            // can be empty.
            if (filteredTopCandidates.size() > 0) {
                currObj = mutuallyConnectNewElement(dataPoint, dataPointInternalId, filteredTopCandidates, level, true);
            }
        }
    }

    std::vector<tableint>
    getConnectionsWithLock(tableint internalId, int level) {
        std::unique_lock<std::mutex> lock(link_list_locks_[internalId]);
        unsigned int* data = get_linklist_at_level(internalId, level);
        int size = getListCount(data);
        std::vector<tableint> result(size);
        tableint* ll = (tableint*)(data + 1);
        memcpy(result.data(), ll, size * sizeof(tableint));
        return result;
    };

    tableint
    addPoint(const void* data_point, labeltype label, int level) {
        tableint cur_c = label;
        {
            std::unique_lock<std::mutex> templock_curr(cur_element_count_guard_);
            if (cur_element_count >= max_elements_) {
                throw std::runtime_error("The number of elements exceeds the specified limit");
            };
            cur_element_count++;
        }

        std::unique_lock<std::mutex> lock_el(link_list_locks_[cur_c]);
        int curlevel = (level > 0) ? level : getRandomLevel(mult_);

        element_levels_[cur_c] = curlevel;

        std::unique_lock<std::mutex> templock(global);
        int maxlevelcopy = maxlevel_;
        if (curlevel <= maxlevelcopy)
            templock.unlock();
        tableint currObj = enterpoint_node_;
        tableint enterpoint_copy = enterpoint_node_;

        memset(data_level0_memory_ + cur_c * size_data_per_element_ + offsetLevel0_, 0, size_data_per_element_);
        if constexpr (has_raw_data) {
            memcpy(getDataByInternalId(cur_c), data_point, data_size_);
            if (metric_type_ == Metric::COSINE) {
                float norm = NormSqr<data_t, dist_t>(data_point, dist_func_param_);
                data_norm_l2_[cur_c] = (norm == 0.0 ? 1.0 : std::sqrt(norm));
            }
        }
        if constexpr (sq_enabled) {
            encodeSQuant((const data_t*)data_point, (int8_t*)getSQDataByInternalId(cur_c));
        }
        if (curlevel) {
            linkLists_[cur_c] = (char*)malloc(size_links_per_element_ * curlevel + 1);
            if (linkLists_[cur_c] == nullptr)
                throw std::runtime_error("Not enough memory: addPoint failed to allocate linklist");
            memset(linkLists_[cur_c], 0, size_links_per_element_ * curlevel + 1);
        }

        if ((signed)currObj != -1) {
            if (curlevel < maxlevelcopy) {
                dist_t curdist = calcDistance(cur_c, currObj);
                for (int level = maxlevelcopy; level > curlevel; level--) {
                    bool changed = true;
                    while (changed) {
                        changed = false;
                        unsigned int* data;
                        std::unique_lock<std::mutex> lock(link_list_locks_[currObj]);
                        data = get_linklist(currObj, level);
                        int size = getListCount(data);

                        tableint* datal = (tableint*)(data + 1);
                        for (int i = 0; i < size; i++) {
                            tableint cand = datal[i];
                            if (cand < 0 || cand > max_elements_)
                                throw std::runtime_error("cand error");
                            dist_t d = calcDistance(cur_c, cand);
                            if (d < curdist) {
                                curdist = d;
                                currObj = cand;
                                changed = true;
                            }
                        }
                    }
                }
            }

            for (int level = std::min(curlevel, maxlevelcopy); level >= 0; level--) {
                if (level > maxlevelcopy || level < 0)  // possible?
                    throw std::runtime_error("Level error");

                std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>,
                                    CompareByFirst>
                    top_candidates = searchBaseLayer(currObj, cur_c, level);
                currObj = mutuallyConnectNewElement(data_point, cur_c, top_candidates, level, false);
            }

        } else {
            // Do nothing for the first element
            enterpoint_node_ = 0;
            maxlevel_ = curlevel;
        }

        // Releasing lock for the maximum level
        if (curlevel > maxlevelcopy) {
            enterpoint_node_ = cur_c;
            maxlevel_ = curlevel;
        }
        return cur_c;
    };

    std::vector<std::pair<dist_t, labeltype>>
    searchKnnBF(const void* query_data, size_t k, const knowhere::BitsetView bitset) const {
        knowhere::ResultMaxHeap<dist_t, labeltype> max_heap(k);
        for (labeltype id = 0; id < cur_element_count; ++id) {
            if (bitset.empty() || !bitset.test(id)) {
                dist_t dist = calcDistance(query_data, id);
                max_heap.Push(dist, id);
            }
        }
        const size_t len = std::min(max_heap.Size(), k);
        std::vector<std::pair<dist_t, labeltype>> result(len);
        for (int64_t i = len - 1; i >= 0; --i) {
            const auto op = max_heap.Pop();
            result[i] = op.value();
        }
        return result;
    }

    std::pair<tableint, int64_t>
    searchTopLayers(const void* query_data, const SearchParam* param = nullptr,
                    const knowhere::feder::hnsw::FederResultUniq& feder_result = nullptr) const {
        tableint currObj = enterpoint_node_;
        uint64_t vec_hash;
        if constexpr (sq_enabled) {
            vec_hash = knowhere::hash_u8_vec((const uint8_t*)query_data, *(size_t*)dist_func_param_);
        } else if constexpr (std::is_same_v<data_t, knowhere::bin1>) {
            vec_hash = knowhere::hash_binary_vec((const uint8_t*)query_data, *(size_t*)dist_func_param_);
        } else if constexpr (std::is_same_v<data_t, knowhere::bf16> || std::is_same_v<data_t, knowhere::fp16>) {
            vec_hash = knowhere::hash_half_precision_float(query_data, *(size_t*)dist_func_param_);
        } else {
            vec_hash = knowhere::hash_vec((const float*)query_data, *(size_t*)dist_func_param_);
        }

        dist_t curdist = calcDistance(query_data, enterpoint_node_);

        if (base_layer_only) {
            for (int i = 0; i < num_seeds; i++) {
                tableint obj = i * (max_elements_ / num_seeds);
                dist_t dist = fstdistfunc_(query_data, getDataByInternalId(obj), dist_func_param_);
                if (dist < curdist) {
                    curdist = dist;
                    currObj = obj;
                }
            }
        } else {
            for (int level = maxlevel_; level > 0; level--) {
                bool changed = true;
                if (feder_result != nullptr) {
                    feder_result->visit_info_.AddLevelVisitRecord(level);
                }
                while (changed) {
                    changed = false;
                    unsigned int* data;

                    data = (unsigned int*)get_linklist(currObj, level);
                    int size = getListCount(data);
                    metric_hops++;
                    metric_distance_computations += size;
                    tableint* datal = (tableint*)(data + 1);
                    for (int i = 0; i < size; ++i) {
                        prefetchData(datal[i]);
                    }
                    for (int i = 0; i < size; i++) {
                        tableint cand = datal[i];
                        if (cand < 0 || cand > max_elements_)
                            throw std::runtime_error("cand error");
                        dist_t d = calcDistance(query_data, cand);
                        if (feder_result != nullptr) {
                            feder_result->visit_info_.AddVisitRecord(level, currObj, cand, d);
                            feder_result->id_set_.insert(currObj);
                            feder_result->id_set_.insert(cand);
                        }

                        if (d < curdist) {
                            curdist = d;
                            currObj = cand;
                            changed = true;
                        }
                    }
                }
            }
        }

        return {currObj, vec_hash};
    }

    std::vector<std::pair<dist_t, labeltype>>
    searchKnn(const void* query_data, size_t k, const knowhere::BitsetView bitset, const SearchParam* param = nullptr,
              const knowhere::feder::hnsw::FederResultUniq& feder_result = nullptr) const {
        if (cur_element_count == 0 || bitset.count() == cur_element_count)
            return {};

        // do normalize for COSINE metric type
        std::unique_ptr<data_t[]> query_data_norm;
        if constexpr (knowhere::KnowhereFloatTypeCheck<data_t>::value) {
            if (metric_type_ == Metric::COSINE) {
                query_data_norm =
                    knowhere::CopyAndNormalizeVecs((const data_t*)query_data, 1, *(size_t*)dist_func_param_);
                query_data = query_data_norm.get();
            }
        }

        std::unique_ptr<int8_t[]> query_data_sq;
        [[maybe_unused]] const data_t* raw_data = (const data_t*)query_data;
        if constexpr (sq_enabled) {
            query_data_sq = std::make_unique<int8_t[]>(*(size_t*)dist_func_param_);
            encodeSQuant((const data_t*)query_data, query_data_sq.get());
            query_data = query_data_sq.get();
        }

        // do bruteforce search when topk is super large
        if (k >= (cur_element_count * kHnswSearchBFTopkThreshold)) {
            return searchKnnBF(query_data, k, bitset);
        }

        // do bruteforce search when delete rate high
        if (!bitset.empty()) {
            const size_t filtered_out_num = bitset.count();
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
            double ratio = ((double)filtered_out_num) / bitset.size();
            knowhere::knowhere_hnsw_bitset_ratio.Observe(ratio);
#endif
            if (filtered_out_num >= (cur_element_count * kHnswSearchKnnBFFilterThreshold) ||
                k >= (cur_element_count - filtered_out_num) * kHnswSearchBFTopkThreshold) {
                return searchKnnBF(query_data, k, bitset);
            }
        }

        auto [currObj, vec_hash] = searchTopLayers(query_data, param, feder_result);
        NeighborSetDoublePopList retset;
        size_t ef = param ? param->ef_ : this->ef_;
        auto visited = visited_list_pool_->getFreeVisitedList();
        if (!bitset.empty()) {
            retset = searchBaseLayerST<true, true>(currObj, query_data, std::max(ef, k), visited, bitset, feder_result);
        } else {
            retset =
                searchBaseLayerST<false, true>(currObj, query_data, std::max(ef, k), visited, bitset, feder_result);
        }
        // switch to brute-force when insufficient k
        if (retset.size() < k) {
            return searchKnnBF(query_data, k, bitset);
        }
        std::vector<std::pair<dist_t, labeltype>> result;
        size_t len = std::min(k, retset.size());
        result.reserve(len);
        if constexpr (sq_enabled && has_raw_data) {
            knowhere::ResultMaxHeap<dist_t, labeltype> max_heap(len);
            for (int i = 0; i < retset.size(); ++i) {
                max_heap.Push(calcRefineDistance(raw_data, retset[i].id), retset[i].id);
            }
            for (int64_t i = len - 1; i >= 0; --i) {
                const auto op = max_heap.Pop();
                result.emplace_back(op.value());
            }
        } else {
            for (int i = 0; i < len; ++i) {
                result.emplace_back(retset[i].distance, (labeltype)retset[i].id);
            }
        }
        return result;
    };

    std::unique_ptr<IteratorWorkspace>
    getIteratorWorkspace(const void* query_data, const size_t ef, const knowhere::BitsetView& bitset) const {
        auto accumulative_alpha = (bitset.count() >= (cur_element_count * kHnswSearchKnnBFFilterThreshold))
                                      ? std::numeric_limits<float>::max()
                                      : 1.0f;
        std::unique_ptr<int8_t[]> query_data_copy = nullptr;
        query_data_copy = std::make_unique<int8_t[]>(data_size_);
        std::memcpy(query_data_copy.get(), query_data, data_size_);
        if constexpr (knowhere::KnowhereFloatTypeCheck<data_t>::value) {
            if (metric_type_ == Metric::COSINE) {
                knowhere::NormalizeVec((data_t*)query_data_copy.get(), *(size_t*)dist_func_param_);
            }
        }

        std::unique_ptr<int8_t[]> query_data_sq = nullptr;
        if constexpr (sq_enabled) {
            query_data_sq = std::make_unique<int8_t[]>(*(size_t*)dist_func_param_);
            encodeSQuant((data_t*)query_data_copy.get(), query_data_sq.get());
        }

        return std::make_unique<IteratorWorkspace>(std::move(query_data_sq), max_elements_, ef,
                                                   std::move(query_data_copy), bitset, accumulative_alpha);
    }

    void
    getIteratorNextBatch(IteratorWorkspace* workspace,
                         const knowhere::feder::hnsw::FederResultUniq& feder_result = nullptr) const {
        workspace->dists.clear();
        if (cur_element_count == 0 || workspace->bitset.count() == cur_element_count) {
            return;
        }
        // TODO: add bruteforce
        auto query_data = workspace->query_data;
        const bool has_deletions = !workspace->bitset.empty();
        if (!workspace->initial_search_done) {
            tableint currObj = searchTopLayers(query_data, workspace->param.get()).first;
            NeighborSetDoublePopList retset;
            if (has_deletions) {
                retset = searchBaseLayerST<true, true>(currObj, query_data, workspace->ef, workspace->visited,
                                                       workspace->bitset, feder_result, &workspace->to_visit,
                                                       workspace->accumulative_alpha);
            } else {
                retset = searchBaseLayerST<false, true>(currObj, query_data, workspace->ef, workspace->visited,
                                                        workspace->bitset, feder_result, &workspace->to_visit,
                                                        workspace->accumulative_alpha);
            }
            workspace->dists.reserve(retset.size());
            for (int i = 0; i < retset.size(); i++) {
                workspace->dists.emplace_back(retset[i].id, retset[i].distance);
            }
            workspace->initial_search_done = true;
            return;
        }
        // TODO: currently each time iterator.Next() is called, we return 1 result but adds more than 1 results to
        // to_visit. Consider limit the size of visit by searching 1 step only after several Next() calls. Careful: how
        // does such strategy affect the correctness of the search?
        while (!workspace->to_visit.empty()) {
            auto top = workspace->to_visit.top();
            workspace->to_visit.pop();
            auto add_search_candidate = [&](Neighbor n) {
                workspace->to_visit.push(n);
                return true;
            };
            if (has_deletions) {
                searchBaseLayerSTNext<decltype(add_search_candidate), true, true>(
                    query_data, top, workspace->visited, workspace->accumulative_alpha, workspace->bitset,
                    add_search_candidate, feder_result);
            } else {
                searchBaseLayerSTNext<decltype(add_search_candidate), false, true>(
                    query_data, top, workspace->visited, workspace->accumulative_alpha, workspace->bitset,
                    add_search_candidate, feder_result);
            }
            if (!has_deletions || !workspace->bitset.test((int64_t)top.id)) {
                workspace->dists.emplace_back(top.id, top.distance);
                return;
            }
        }
    }

    std::vector<std::pair<dist_t, labeltype>>
    searchRangeBF(const void* query_data, float radius, const knowhere::BitsetView bitset) const {
        std::vector<std::pair<dist_t, labeltype>> result;
        for (labeltype id = 0; id < cur_element_count; ++id) {
            if (bitset.empty() || !bitset.test(id)) {
                dist_t dist = calcDistance(query_data, id);
                if (dist < radius) {
                    result.emplace_back(dist, id);
                }
            }
        }
        return result;
    }

    std::vector<std::pair<dist_t, labeltype>>
    searchRange(const void* query_data, float radius, const knowhere::BitsetView bitset,
                const SearchParam* param = nullptr,
                const knowhere::feder::hnsw::FederResultUniq& feder_result = nullptr) const {
        if (cur_element_count == 0 || bitset.count() == cur_element_count) {
            return {};
        }

        // do normalize for COSINE metric type
        std::unique_ptr<data_t[]> query_data_norm;
        if constexpr (knowhere::KnowhereFloatTypeCheck<data_t>::value) {
            if (metric_type_ == Metric::COSINE) {
                query_data_norm =
                    knowhere::CopyAndNormalizeVecs((const data_t*)query_data, 1, *(size_t*)dist_func_param_);
                query_data = query_data_norm.get();
            }
        }

        std::unique_ptr<int8_t[]> query_data_sq;
        if constexpr (sq_enabled) {
            query_data_sq = std::make_unique<int8_t[]>(*(size_t*)dist_func_param_);
            encodeSQuant((const data_t*)query_data, query_data_sq.get());
            query_data = query_data_sq.get();
        }

        // do bruteforce range search when ef is super large
        size_t ef = param ? param->ef_ : this->ef_;
        if (ef >= (cur_element_count * kHnswSearchBFTopkThreshold)) {
            return searchRangeBF(query_data, radius, bitset);
        }

        // do bruteforce range search when delete rate high
        if (!bitset.empty()) {
            const size_t filtered_out_num = bitset.count();
#if defined(NOT_COMPILE_FOR_SWIG) && !defined(KNOWHERE_WITH_LIGHT)
            double ratio = ((double)filtered_out_num) / bitset.size();
            knowhere::knowhere_hnsw_bitset_ratio.Observe(ratio);
#endif
            if (filtered_out_num >= (cur_element_count * kHnswSearchRangeBFFilterThreshold) ||
                ef >= (cur_element_count - filtered_out_num) * kHnswSearchBFTopkThreshold) {
                return searchRangeBF(query_data, radius, bitset);
            }
        }

        auto [currObj, vec_hash] = searchTopLayers(query_data, param, feder_result);
        NeighborSetDoublePopList retset;
        auto visited = visited_list_pool_->getFreeVisitedList();
        if (!bitset.empty()) {
            retset = searchBaseLayerST<true, true>(currObj, query_data, ef, visited, bitset, feder_result);
        } else {
            retset = searchBaseLayerST<false, true>(currObj, query_data, ef, visited, bitset, feder_result);
        }

        if (retset.size() == 0) {
            return {};
        }

        return getNeighboursWithinRadius(retset, query_data, radius, bitset);
    }

    // get those unreachable vectors at the base layer after index building
    // only be called after index building
    std::vector<tableint>
    findUnreachableVectors() {
        tableint currObj = enterpoint_node_;
        std::vector<tableint> start_points;
        start_points.push_back(currObj);
        std::vector<bool> visited;
        std::vector<tableint> unreached;
        for (int level = maxlevel_; level >= 0; level--) {
            visited = std::vector<bool>(cur_element_count, false);
            std::vector<tableint> touched;
            for (auto start_point : start_points) {
                if (visited[start_point])
                    continue;
                std::queue<tableint> q;
                q.push(start_point);
                visited[start_point] = true;
                if (level > 0)
                    touched.push_back(start_point);
                while (!q.empty()) {
                    tableint j = q.front();
                    q.pop();
                    unsigned int* data;
                    data = (unsigned int*)get_linklist_at_level(j, level);
                    size_t size = getListCount((linklistsizeint*)data);
                    tableint* datal = (tableint*)(data + 1);
                    for (size_t k = 0; k < size; k++) {
                        tableint cand = datal[k];
                        if (!visited[cand]) {
                            visited[cand] = true;
                            q.push(cand);
                            if (level > 0)
                                touched.push_back(cand);
                        }
                    }
                }
            }
            start_points = touched;

            for (tableint i = 0; i < cur_element_count; ++i) {
                if (element_levels_[i] >= level) {
                    if (!visited[i]) {
                        if (level > 0) {  // for upper level, directly add edges since nodes num is usually small and
                                          // fast to search its neighbors
                            repairGraphConnectivity(i, level);
                        } else {  // for base level, collect the unreachable nodes and repair them concurrently
                            unreached.push_back(i);
                        }
                    }
                }
            }
        }
        return unreached;
    }

    // add some edges for those unreachable vectors to improve graph connectivity
    // only call this method after index building
    void
    repairGraphConnectivity(tableint cur_c, int level = 0) {
        size_t m_max = level ? maxM_ : maxM0_;
        tableint currObj = enterpoint_node_;

        dist_t curdist = calcDistance(cur_c, currObj);

        for (int level_above = maxlevel_; level_above > level; level_above--) {
            bool changed = true;
            while (changed) {
                changed = false;
                unsigned int* data;
                // do not a lock here, since upper layer will not be modified
                data = (unsigned int*)get_linklist(currObj, level_above);
                int size = getListCount(data);
                tableint* datal = (tableint*)(data + 1);
#if defined(USE_PREFETCH)
                for (int i = 0; i < size; ++i) {
                    _mm_prefetch(getDataByInternalId(datal[i]), _MM_HINT_T0);
                }
#endif
                for (int i = 0; i < size; i++) {
                    tableint cand = datal[i];
                    if (cand < 0 || cand > max_elements_)
                        throw std::runtime_error("cand error");
                    dist_t d = calcDistance(cur_c, cand);

                    if (d < curdist) {
                        curdist = d;
                        currObj = cand;
                        changed = true;
                    }
                }
            }
        }
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            candidates = searchBaseLayer(currObj, cur_c, level);

        // get sorted id
        std::vector<tableint> top_candidate_ids(candidates.size());
        for (int i = static_cast<int>(candidates.size() - 1); i >= 0; i--) {
            top_candidate_ids[i] = candidates.top().second;
            candidates.pop();
        }
        int add_count = 0;
        for (auto cand_id : top_candidate_ids) {
            // skip same element
            if (cand_id == cur_c) {
                continue;
            }

            // try to connect candidate to the element
            // add an edge if there is space
            std::unique_lock<std::mutex> lock(link_list_locks_[cand_id]);
            linklistsizeint* ll_cand = get_linklist_at_level(cand_id, level);
            size_t size = getListCount(ll_cand);
            tableint* data_cand = (tableint*)(ll_cand + 1);
            if (size < m_max) {
                data_cand[size] = cur_c;
                setListCount(ll_cand, size + 1);
                add_count++;
            }
            // do not add too much? If we already have m_max nodes connecting to the element
            if (add_count >= m_max) {
                break;
            }
        }
    }

    void
    checkIntegrity() {
        int connections_checked = 0;
        std::vector<int> inbound_connections_num(cur_element_count, 0);
        for (int i = 0; i < cur_element_count; i++) {
            for (int l = 0; l <= element_levels_[i]; l++) {
                linklistsizeint* ll_cur = get_linklist_at_level(i, l);
                int size = getListCount(ll_cur);
                tableint* data = (tableint*)(ll_cur + 1);
                std::unordered_set<tableint> s;
                for (int j = 0; j < size; j++) {
                    assert(data[j] > 0);
                    assert(data[j] < cur_element_count);
                    assert(data[j] != i);
                    inbound_connections_num[data[j]]++;
                    s.insert(data[j]);
                    connections_checked++;
                }
                assert(s.size() == size);
            }
        }
        if (cur_element_count > 1) {
            int min1 = inbound_connections_num[0], max1 = inbound_connections_num[0];
            for (int i = 0; i < cur_element_count; i++) {
                assert(inbound_connections_num[i] > 0);
                min1 = std::min(inbound_connections_num[i], min1);
                max1 = std::max(inbound_connections_num[i], max1);
            }
            std::cout << "Min inbound: " << min1 << ", Max inbound:" << max1 << "\n";
        }
        std::cout << "integrity ok, checked " << connections_checked << " connections\n";
    }

    int64_t
    cal_size() {
        int64_t ret = 0;
        ret += sizeof(*this);
        ret += sizeof(*space_);
        ret += visited_list_pool_->size();
        ret += element_levels_.size() * sizeof(int);
        ret += max_elements_ * size_data_per_element_;
        ret += max_elements_ * sizeof(void*);
        for (auto i = 0; i < max_elements_; ++i) {
            if (element_levels_[i] > 0) {
                ret += size_links_per_element_ * element_levels_[i];
            }
        }
        if (metric_type_ == Metric::COSINE) {
            ret += max_elements_ * sizeof(float);
        }
        return ret;
    }
};

}  // namespace hnswlib
