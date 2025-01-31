/**
 * SPDX-FileCopyrightText: Copyright (c) 2023,NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <thrust/device_ptr.h>
#include <thrust/fill.h>
#include <thrust/for_each.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/zip_iterator.h>
#include <thrust/partition.h>
#include <thrust/tuple.h>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <raft/core/bitmap.cuh>
#include <raft/core/bitset.cuh>
#include <raft/core/logger.hpp>
#include <raft/core/copy.cuh>
#include <cuvs/neighbors/brute_force.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include <cuvs/neighbors/ivf_flat.hpp>
#include <cuvs/neighbors/ivf_pq.hpp>
#include <cuvs/neighbors/refine.hpp>
#include <type_traits>

#include "common/raft/proto/raft_index_kind.hpp"

namespace raft_proto {

auto static const RAFT_NAME = "RAFT";

namespace detail {
template <raft_index_kind index_kind, template <typename...> typename index_template>
struct template_matches_index_kind : std::false_type {};

template <>
struct template_matches_index_kind<raft_index_kind::brute_force, cuvs::neighbors::brute_force::index> : std::true_type {
};

template <>
struct template_matches_index_kind<raft_index_kind::ivf_flat, cuvs::neighbors::ivf_flat::index> : std::true_type {};

template <>
struct template_matches_index_kind<raft_index_kind::ivf_pq, cuvs::neighbors::ivf_pq::index> : std::true_type {};

template <>
struct template_matches_index_kind<raft_index_kind::cagra, cuvs::neighbors::cagra::index> : std::true_type {};

template <raft_index_kind index_kind, template <typename...> typename index_template>
auto static constexpr template_matches_index_kind_v = template_matches_index_kind<index_kind, index_template>::value;

// Note: The following are not at all general or properly SFINAE-guarded. They
// should be replaced if the post_filter proto is required for upstreaming.
template <typename mdspan_t>
auto
mdspan_begin(mdspan_t data) {
    return thrust::device_ptr<typename mdspan_t::value_type>(data.data_handle());
}
template <typename mdspan_t>
auto
mdspan_end(mdspan_t data) {
    return thrust::device_ptr<typename mdspan_t::value_type>(data.data_handle() + data.size());
}

template <typename mdspan_t>
auto
mdspan_begin_row(mdspan_t data, std::size_t row) {
    return thrust::device_ptr<typename mdspan_t::value_type>(data.data_handle() + row * data.extent(1));
}
template <typename mdspan_t>
auto
mdspan_end_row(mdspan_t data, std::size_t row) {
    return thrust::device_ptr<typename mdspan_t::value_type>(data.data_handle() + (row + 1) * data.extent(1));
}

template <typename index_mdspan_t, typename distance_mdspan_t, typename filter_lambda_t>
void
post_filter(raft::resources const& res, filter_lambda_t const& sample_filter, index_mdspan_t index_mdspan,
            distance_mdspan_t distance_mdspan) {
    auto counter = thrust::counting_iterator<decltype(index_mdspan.extent(0))>(0);
    // TODO (wphicks): This could be rolled into the stable_partition calls
    // below, but I am not sure whether or not that would be a net benefit. This
    // deserves some benchmarking unless pre-filtering gets in before we revisit
    // this.
    thrust::for_each(
        raft::resource::get_thrust_policy(res),
        thrust::make_zip_iterator(
            thrust::make_tuple(counter, mdspan_begin(index_mdspan), mdspan_begin(distance_mdspan))),
        thrust::make_zip_iterator(
            thrust::make_tuple(counter + index_mdspan.size(), mdspan_end(index_mdspan), mdspan_end(distance_mdspan))),
        [=] __device__(const thrust::tuple<decltype(index_mdspan.extent(0)), typename index_mdspan_t::element_type&,
                                           typename distance_mdspan_t::element_type&>& index_id_distance) {
            auto index = thrust::get<0>(index_id_distance);
            auto& id = thrust::get<1>(index_id_distance);
            auto& distance = thrust::get<2>(index_id_distance);
            //if (!sample_filter(index / index_mdspan.extent(1), id)) {
            if (!sample_filter.bitset_view_.test(id)) {
                id = std::numeric_limits<std::remove_reference_t<decltype(id)>>::max();
                distance = std::numeric_limits<std::remove_reference_t<decltype(distance)>>::max();
            }
        });
    for (auto i = 0; i < index_mdspan.extent(0); ++i) {
        auto id_row_begin = mdspan_begin_row(index_mdspan, i);
        auto id_row_end = mdspan_end_row(index_mdspan, i);
        auto distance_row_begin = mdspan_begin_row(distance_mdspan, i);
        auto distance_row_end = mdspan_end_row(distance_mdspan, i);
        thrust::stable_partition(
            raft::resource::get_thrust_policy(res),
            thrust::make_zip_iterator(thrust::make_tuple(id_row_begin, distance_row_begin)),
            thrust::make_zip_iterator(thrust::make_tuple(id_row_end, distance_row_end)),
            [=] __device__(auto& id_distance) {
                return thrust::get<0>(id_distance) !=
                       std::numeric_limits<std::remove_reference_t<decltype(thrust::get<0>(id_distance))>>::max();
            });
    }
}

template <typename T, typename IdxT>
void
serialize_to_hnswlib(raft::resources const& res, std::ostream& os,
                     const cuvs::neighbors::cagra::index<T, IdxT>& index_) {
    size_t metric_type;
    if (index_.metric() == cuvs::distance::DistanceType::L2Expanded) {
        metric_type = 0;
    } else if (index_.metric() == cuvs::distance::DistanceType::InnerProduct) {
        metric_type = 1;
    } else if (index_.metric() == cuvs::distance::DistanceType::CosineExpanded) {
        metric_type = 2;
    }

    os.write(reinterpret_cast<char*>(&metric_type), sizeof(metric_type));
    size_t data_size = index_.dim() * sizeof(float);
    os.write(reinterpret_cast<char*>(&data_size), sizeof(data_size));
    size_t dim = index_.dim();
    os.write(reinterpret_cast<char*>(&dim), sizeof(dim));
    std::size_t offset_level_0 = 0;
    os.write(reinterpret_cast<char*>(&offset_level_0), sizeof(std::size_t));
    std::size_t max_element = index_.size();
    os.write(reinterpret_cast<char*>(&max_element), sizeof(std::size_t));
    std::size_t curr_element_count = index_.size();
    os.write(reinterpret_cast<char*>(&curr_element_count), sizeof(std::size_t));
    auto size_data_per_element =
        static_cast<std::size_t>(index_.graph_degree() * sizeof(IdxT) + 4 + index_.dim() * sizeof(T) + 8);
    os.write(reinterpret_cast<char*>(&size_data_per_element), sizeof(std::size_t));
    std::size_t label_offset = size_data_per_element - 8;
    os.write(reinterpret_cast<char*>(&label_offset), sizeof(std::size_t));
    auto offset_data = static_cast<std::size_t>(index_.graph_degree() * sizeof(IdxT) + 4);
    os.write(reinterpret_cast<char*>(&offset_data), sizeof(std::size_t));
    int max_level = 1;
    os.write(reinterpret_cast<char*>(&max_level), sizeof(int));
    auto entrypoint_node = static_cast<int>(index_.size() / 2);
    os.write(reinterpret_cast<char*>(&entrypoint_node), sizeof(int));
    auto max_M = static_cast<std::size_t>(index_.graph_degree() / 2);
    os.write(reinterpret_cast<char*>(&max_M), sizeof(std::size_t));
    std::size_t max_M0 = index_.graph_degree();
    os.write(reinterpret_cast<char*>(&max_M0), sizeof(std::size_t));
    auto M = static_cast<std::size_t>(index_.graph_degree() / 2);
    os.write(reinterpret_cast<char*>(&M), sizeof(std::size_t));
    double mult = 0.42424242;
    os.write(reinterpret_cast<char*>(&mult), sizeof(double));
    std::size_t efConstruction = 500;
    os.write(reinterpret_cast<char*>(&efConstruction), sizeof(std::size_t));

    auto dataset = index_.dataset();
    auto host_dataset = raft::make_host_matrix<T, int64_t>(dataset.extent(0), dataset.extent(1));
    RAFT_CUDA_TRY(cudaMemcpy2DAsync(host_dataset.data_handle(), sizeof(T) * host_dataset.extent(1),
                                    dataset.data_handle(), sizeof(T) * dataset.stride(0),
                                    sizeof(T) * host_dataset.extent(1), dataset.extent(0), cudaMemcpyDefault,
                                    raft::resource::get_cuda_stream(res)));
    raft::resource::sync_stream(res);

    auto graph = index_.graph();
    auto host_graph = raft::make_host_matrix<IdxT, int64_t, raft::row_major>(graph.extent(0), graph.extent(1));
    raft::copy(host_graph.data_handle(), graph.data_handle(), graph.size(), raft::resource::get_cuda_stream(res));
    raft::resource::sync_stream(res);

    for (std::size_t i = 0; i < index_.size(); i++) {
        auto graph_degree = static_cast<uint32_t>(index_.graph_degree());
        os.write(reinterpret_cast<char*>(&graph_degree), sizeof(uint32_t));

        for (std::size_t j = 0; j < index_.graph_degree(); ++j) {
            auto graph_elem = host_graph(i, j);
            os.write(reinterpret_cast<char*>(&graph_elem), sizeof(IdxT));
        }

        auto data_row = host_dataset.data_handle() + (index_.dim() * i);
        for (std::size_t j = 0; j < index_.dim(); ++j) {
            auto data_elem = host_dataset(i, j);
            os.write(reinterpret_cast<char*>(&data_elem), sizeof(T));
        }

        os.write(reinterpret_cast<char*>(&i), sizeof(std::size_t));
    }

    for (std::size_t i = 0; i < index_.size(); i++) {
        // zeroes
        auto zero = 0;
        os.write(reinterpret_cast<char*>(&zero), sizeof(int));
    }
    os.flush();
}

}  // namespace detail

template <template <typename...> typename underlying_index_type, typename... raft_index_args>
struct raft_index {
    using vector_index_type = underlying_index_type<raft_index_args...>;
    auto static constexpr vector_index_kind = []() {
        if constexpr (detail::template_matches_index_kind_v<raft_index_kind::brute_force, underlying_index_type>) {
            return raft_index_kind::brute_force;
        } else if constexpr (detail::template_matches_index_kind_v<raft_index_kind::ivf_flat, underlying_index_type>) {
            return raft_index_kind::ivf_flat;
        } else if constexpr (detail::template_matches_index_kind_v<raft_index_kind::ivf_pq, underlying_index_type>) {
            return raft_index_kind::ivf_pq;
        } else if constexpr (detail::template_matches_index_kind_v<raft_index_kind::cagra, underlying_index_type>) {
            return raft_index_kind::cagra;
        } else {
            static_assert(detail::template_matches_index_kind_v<raft_index_kind::brute_force, underlying_index_type>,
                          "Unsupported index template passed to raft_index");
        }
    }();

    using index_params_type = std::conditional_t<
        vector_index_kind == raft_index_kind::brute_force, cuvs::neighbors::brute_force::index_params,
        std::conditional_t<
            vector_index_kind == raft_index_kind::ivf_flat, cuvs::neighbors::ivf_flat::index_params,
            std::conditional_t<
                vector_index_kind == raft_index_kind::ivf_pq, cuvs::neighbors::ivf_pq::index_params,
                std::conditional_t<vector_index_kind == raft_index_kind::cagra, cuvs::neighbors::cagra::index_params,
                                   // Should never get here; precluded by static assertion above
                                   cuvs::neighbors::index_params>>>>;
    using search_params_type = std::conditional_t<
        vector_index_kind == raft_index_kind::brute_force, cuvs::neighbors::brute_force::search_params,
        std::conditional_t<
            vector_index_kind == raft_index_kind::ivf_flat, cuvs::neighbors::ivf_flat::search_params,
            std::conditional_t<
                vector_index_kind == raft_index_kind::ivf_pq, cuvs::neighbors::ivf_pq::search_params,
                std::conditional_t<vector_index_kind == raft_index_kind::cagra, cuvs::neighbors::cagra::search_params,
                                   // Should never get here; precluded by static assertion above
                                   cuvs::neighbors::search_params>>>>;

    [[nodiscard]] auto&
    get_vector_index() {
        return vector_index_;
    }
    [[nodiscard]] auto const&
    get_vector_index() const {
        return vector_index_;
    }
    [[nodiscard]] auto
    size() const {
        return vector_index_.size();
    }
    [[nodiscard]] auto
    dim() const {
        return vector_index_.dim();
    }

    template <typename T, typename IdxT, typename InputIdxT, typename DataMdspanT>
    auto static build(raft::resources const& res, index_params_type const& index_params, DataMdspanT data) {
        if constexpr (std::is_same_v<DataMdspanT, raft::host_matrix_view<T const, IdxT>>) {
            if constexpr (vector_index_kind == raft_index_kind::brute_force) {
                return raft_index<underlying_index_type, raft_index_args...>{
                    cuvs::neighbors::brute_force::build(res, index_params, data)};
            } else if constexpr (vector_index_kind == raft_index_kind::cagra) {
                return raft_index<underlying_index_type, raft_index_args...>{
                    cuvs::neighbors::cagra::build(res, index_params, data)};
            } else if constexpr (vector_index_kind == raft_index_kind::ivf_pq) {
                return raft_index<underlying_index_type, raft_index_args...>{cuvs::neighbors::ivf_pq::build(
                    res, index_params, data)};
            } else if constexpr (vector_index_kind == raft_index_kind::ivf_flat) {
                return raft_index<underlying_index_type, raft_index_args...>{
                    cuvs::neighbors::ivf_flat::build(res, index_params, data)};
            }
        } else {
            if constexpr (vector_index_kind == raft_index_kind::brute_force) {
                return raft_index<underlying_index_type, raft_index_args...>{
                    cuvs::neighbors::brute_force::build(res, data, index_params.metric, index_params.metric_arg)};
            } else if constexpr (vector_index_kind == raft_index_kind::ivf_flat) {
                return raft_index<underlying_index_type, raft_index_args...>{
                    cuvs::neighbors::ivf_flat::build(res, index_params, data)};
            } else if constexpr (vector_index_kind == raft_index_kind::ivf_pq) {
                return raft_index<underlying_index_type, raft_index_args...>{
                    cuvs::neighbors::ivf_pq::build(res, index_params, data)};
            } else if constexpr (vector_index_kind == raft_index_kind::cagra) {
                return raft_index<underlying_index_type, raft_index_args...>{
                    cuvs::neighbors::cagra::build(res, index_params, data)};
            }
        }
    }

    template <typename T, typename IdxT, typename InputIdxT, typename FilterT = cuvs::neighbors::filtering::none_sample_filter>
    auto static search(raft::resources const& res, raft_index<underlying_index_type, raft_index_args...> const& index,
                       search_params_type const& search_params, raft::device_matrix_view<T const, InputIdxT> queries,
                       raft::device_matrix_view<IdxT, InputIdxT> neighbors,
                       raft::device_matrix_view<float, InputIdxT> distances, float refine_ratio = 1.0f,
                       InputIdxT k_offset = InputIdxT{},
                       std::optional<raft::device_matrix_view<const T, InputIdxT>> dataset = std::nullopt,
                       FilterT filter = cuvs::neighbors::filtering::none_sample_filter{}) {
        auto const& underlying_index = index.get_vector_index();

        auto k = neighbors.extent(1);
        auto k_tmp = k + k_offset;
        if (refine_ratio > 1.0f) {
            k_tmp *= refine_ratio;
        }

        auto neighbors_tmp = neighbors;
        auto distances_tmp = distances;
        auto neighbors_storage = std::optional<raft::device_matrix<IdxT, InputIdxT>>{};
        auto distances_storage = std::optional<raft::device_matrix<float, InputIdxT>>{};

        if (k_tmp > k) {
            neighbors_storage = raft::make_device_matrix<IdxT, InputIdxT>(res, queries.extent(0), k_tmp);
            neighbors_tmp = neighbors_storage->view();
            distances_storage = raft::make_device_matrix<float, InputIdxT>(res, queries.extent(0), k_tmp);
            distances_tmp = distances_storage->view();
        }

        if constexpr (vector_index_kind == raft_index_kind::brute_force) {
            cuvs::neighbors::brute_force::search(res, underlying_index, queries, neighbors_tmp,
                                                 distances_tmp);
            if constexpr (!std::is_same_v<FilterT, cuvs::neighbors::filtering::none_sample_filter>) {
                // TODO(wphicks): This can be replaced once prefiltering is
                // implemented for brute force upstream
                detail::post_filter(res, filter, neighbors_tmp, distances_tmp);
            }
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_flat) {
            cuvs::neighbors::ivf_flat::search(res, search_params, underlying_index, queries, neighbors_tmp,
                                              distances_tmp, filter);
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_pq) {
            cuvs::neighbors::ivf_pq::search(res, search_params, underlying_index, queries, neighbors_tmp,
                                            distances_tmp, filter);
        } else if constexpr (vector_index_kind == raft_index_kind::cagra) {
            cuvs::neighbors::cagra::search(res, search_params, underlying_index, queries, neighbors_tmp,
                                           distances_tmp, filter);
        }
        if (refine_ratio > 1.0f) {
            if (dataset.has_value()) {
                if constexpr (std::is_same_v<IdxT, InputIdxT>) {
                    cuvs::neighbors::refine(res, *dataset, queries, raft::make_const_mdspan(neighbors_tmp), neighbors,
                                            distances, underlying_index.metric());
                } else {
                    cuvs::neighbors::refine(
                        res,
                        raft::make_device_matrix_view(dataset->data_handle(), InputIdxT(dataset->extent(0)),
                                                      InputIdxT(dataset->extent(1))),
                        raft::make_device_matrix_view(queries.data_handle(), InputIdxT(queries.extent(0)),
                                                      InputIdxT(queries.extent(1))),
                        raft::make_const_mdspan(raft::make_device_matrix_view(
                            neighbors_tmp.data_handle(), InputIdxT(neighbors_tmp.extent(0)), InputIdxT(neighbors_tmp.extent(1)))),
                        raft::make_device_matrix_view(neighbors.data_handle(), InputIdxT(neighbors.extent(0)),
                                                      InputIdxT(neighbors.extent(1))),
                        raft::make_device_matrix_view(distances.data_handle(), InputIdxT(distances.extent(0)),
                                                      InputIdxT(distances.extent(1))),
                        underlying_index.metric());
                }
            } else {
                RAFT_LOG_WARN("Refinement requested, but no dataset provided. Ignoring refinement request.");
            }
        }
    }

    template <typename T, typename IdxT, typename InputIdxT>
    auto static extend(raft::resources const& res, raft::device_matrix_view<T const, InputIdxT> new_vectors,
                       std::optional<raft::device_vector_view<IdxT const, InputIdxT>> new_ids,
                       raft_index<underlying_index_type, raft_index_args...>& index) {
        auto const& underlying_index = index.get_vector_index();

        if constexpr (vector_index_kind == raft_index_kind::brute_force) {
            // TODO(wphicks): Implement brute force extend
            RAFT_FAIL("Brute force implements no extend method");
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_flat) {
            return raft_index{cuvs::neighbors::ivf_flat::extend(res, new_vectors, new_ids, underlying_index)};
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_pq) {
            return raft_index{cuvs::neighbors::ivf_pq::extend(res, new_vectors, new_ids, underlying_index)};
        } else if constexpr (vector_index_kind == raft_index_kind::cagra) {
            RAFT_FAIL("CAGRA implements no extend method");
        }
    }

    template <typename T, typename IdxT>
    void static serialize(raft::resources const& res, std::ostream& os,
                          raft_index<underlying_index_type, raft_index_args...> const& index,
                          bool include_dataset = true) {
        auto const& underlying_index = index.get_vector_index();

        if constexpr (vector_index_kind == raft_index_kind::brute_force) {
            return cuvs::neighbors::brute_force::serialize(res, os, underlying_index, include_dataset);
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_flat) {
            return cuvs::neighbors::ivf_flat::serialize(res, os, underlying_index);
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_pq) {
            return cuvs::neighbors::ivf_pq::serialize(res, os, underlying_index);
        } else if constexpr (vector_index_kind == raft_index_kind::cagra) {
            return cuvs::neighbors::cagra::serialize(res, os, underlying_index, include_dataset);
        }
    }

    template <typename T, typename IdxT>
    void static serialize_to_hnswlib(raft::resources const& res, std::ostream& os,
                                     raft_index<underlying_index_type, raft_index_args...> const& index,
                                     bool include_dataset = true) {
        auto const& underlying_index = index.get_vector_index();
        if constexpr (vector_index_kind == raft_index_kind::cagra) {
            // TODO(mide): Use cuvs function
            return raft_proto::detail::serialize_to_hnswlib<T, IdxT>(res, os, underlying_index);
        }
    }

    template <typename T, typename IdxT>
    auto static deserialize(raft::resources const& res, std::istream& is) {
        if constexpr (vector_index_kind == raft_index_kind::brute_force) {
            cuvs::neighbors::brute_force::index<T, float> loaded_index(res);
            cuvs::neighbors::brute_force::deserialize(res, is, &loaded_index);
            return raft_index{std::forward<decltype(loaded_index)>(loaded_index)};
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_flat) {
            cuvs::neighbors::ivf_flat::index<T, IdxT> loaded_index(res);
            cuvs::neighbors::ivf_flat::deserialize(res, is, &loaded_index);
            return raft_index{std::forward<decltype(loaded_index)>(loaded_index)};
        } else if constexpr (vector_index_kind == raft_index_kind::ivf_pq) {
            cuvs::neighbors::ivf_pq::index<IdxT> loaded_index(res);
            cuvs::neighbors::ivf_pq::deserialize(res, is, &loaded_index);
            return raft_index{std::forward<decltype(loaded_index)>(loaded_index)};
        } else if constexpr (vector_index_kind == raft_index_kind::cagra) {
            cuvs::neighbors::cagra::index<T, IdxT> loaded_index(res);
            cuvs::neighbors::cagra::deserialize(res, is, &loaded_index);
            return raft_index{std::forward<decltype(loaded_index)>(loaded_index)};
        }
    }

    template <typename T, typename InputIdxT>
    void static update_dataset(raft::resources const& res, raft_index<underlying_index_type, raft_index_args...>& index,
                               raft::device_matrix_view<T const, InputIdxT> data) {
        if constexpr (vector_index_kind == raft_index_kind::brute_force ||
                      vector_index_kind == raft_index_kind::cagra) {
            index.get_vector_index().update_dataset(res, data);
        } else {
            RAFT_FAIL("update_dataset is not supported for this index type");
        }
    }

 private:
    vector_index_type vector_index_;

    explicit raft_index(vector_index_type&& vector_index) : vector_index_{std::move(vector_index)} {
    }
};

}  // namespace raft_proto
