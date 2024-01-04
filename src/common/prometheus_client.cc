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

#include "knowhere/prometheus_client.h"

namespace knowhere {

const prometheus::Histogram::BucketBoundaries defaultBuckets = {
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 1048576};

const prometheus::Histogram::BucketBoundaries ratioBuckets = {
    0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95, 1.0};

const std::unique_ptr<PrometheusClient> prometheusClient = std::make_unique<PrometheusClient>();

/*******************************************************************************
 * !!! NOT use SUMMARY metrics here, because when parse SUMMARY metrics in Milvus,
 *     see following error:
 *
 *   An error has occurred while serving metrics:
 *   text format parsing error in line 50: expected float as value, got "=\"0.9\"}"
 ******************************************************************************/
DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(build_latency, "index build latency (s)")
DEFINE_PROMETHEUS_HISTOGRAM(build_latency, PROMETHEUS_LABEL_KNOWHERE)
DEFINE_PROMETHEUS_HISTOGRAM(build_latency, PROMETHEUS_LABEL_CARDINAL)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(load_latency, "index load latency (ms)")
DEFINE_PROMETHEUS_HISTOGRAM(load_latency, PROMETHEUS_LABEL_KNOWHERE)
DEFINE_PROMETHEUS_HISTOGRAM(load_latency, PROMETHEUS_LABEL_CARDINAL)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(search_latency, "search latency (ms)")
DEFINE_PROMETHEUS_HISTOGRAM(search_latency, PROMETHEUS_LABEL_KNOWHERE)
DEFINE_PROMETHEUS_HISTOGRAM(search_latency, PROMETHEUS_LABEL_CARDINAL)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(range_search_latency, "range search latency (ms)")
DEFINE_PROMETHEUS_HISTOGRAM(range_search_latency, PROMETHEUS_LABEL_KNOWHERE)
DEFINE_PROMETHEUS_HISTOGRAM(range_search_latency, PROMETHEUS_LABEL_CARDINAL)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(ann_iterator_init_latency, "ann iterator init latency (ms)")
DEFINE_PROMETHEUS_HISTOGRAM(ann_iterator_init_latency, PROMETHEUS_LABEL_KNOWHERE)
DEFINE_PROMETHEUS_HISTOGRAM(ann_iterator_init_latency, PROMETHEUS_LABEL_CARDINAL)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(search_topk, "search topk")
DEFINE_PROMETHEUS_HISTOGRAM(search_topk, PROMETHEUS_LABEL_KNOWHERE)
DEFINE_PROMETHEUS_HISTOGRAM(search_topk, PROMETHEUS_LABEL_CARDINAL)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(bitset_ratio, "bitset ratio")
DEFINE_PROMETHEUS_HISTOGRAM_WITH_BUCKETS(bitset_ratio, PROMETHEUS_LABEL_CARDINAL, ratioBuckets)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(hnsw_bitset_ratio, "HNSW bitset ratio for search and range search")
DEFINE_PROMETHEUS_HISTOGRAM_WITH_BUCKETS(hnsw_bitset_ratio, PROMETHEUS_LABEL_KNOWHERE, ratioBuckets)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(hnsw_search_hops, "HNSW search hops in layer 0")
DEFINE_PROMETHEUS_HISTOGRAM(hnsw_search_hops, PROMETHEUS_LABEL_KNOWHERE)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(diskann_bitset_ratio, "DISKANN bitset ratio for search and range search")
DEFINE_PROMETHEUS_HISTOGRAM_WITH_BUCKETS(diskann_bitset_ratio, PROMETHEUS_LABEL_KNOWHERE, ratioBuckets)

DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(diskann_search_hops, "DISKANN search hops")
DEFINE_PROMETHEUS_HISTOGRAM(diskann_search_hops, PROMETHEUS_LABEL_KNOWHERE)

const prometheus::Histogram::BucketBoundaries diskannRangeSearchIterBuckets = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22};
DEFINE_PROMETHEUS_HISTOGRAM_FAMILY(diskann_range_search_iters, "DISKANN range search iterations")
DEFINE_PROMETHEUS_HISTOGRAM_WITH_BUCKETS(diskann_range_search_iters, PROMETHEUS_LABEL_KNOWHERE,
                                         diskannRangeSearchIterBuckets)

}  // namespace knowhere
