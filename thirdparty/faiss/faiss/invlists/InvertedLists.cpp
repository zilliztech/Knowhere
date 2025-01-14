/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <faiss/invlists/InvertedLists.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <numeric>

#include <faiss/impl/FaissAssert.h>
#include <faiss/utils/distances.h>
#include <faiss/utils/utils.h>

//TODO: refactor to decouple dependency between CPU and Cuda, or upgrade faiss
#ifdef USE_GPU
#include "faiss/gpu/utils/DeviceUtils.h"
#include "cuda.h"
#include "cuda_runtime.h"

namespace faiss {

/*
 * Use pin memory to build Readonly Inverted list will accelerate cuda memory copy, but it will downgrade cpu ivf search
 * performance. read only inverted list structure will also make ivf search performance not stable. ISSUE 500 mention
 * this problem. Best performance is the original inverted list with non pin memory.
 */

PageLockMemory::PageLockMemory(size_t size) : nbytes(size) {
    auto err = cudaHostAlloc(&(this->data), size, 0);
    if (err) {
        std::string msg =
            "Fail to alloc page lock memory " + std::to_string(size) + ", err code " + std::to_string((int32_t)err);
        FAISS_THROW_MSG(msg);
    }
}

PageLockMemory::~PageLockMemory() {
    CUDA_VERIFY(cudaFreeHost((void*)(this->data)));
}

PageLockMemory::PageLockMemory(const PageLockMemory& other) {
    auto err = cudaHostAlloc(&(this->data), other.nbytes, 0);
    if (err) {
        std::string msg = "Fail to alloc page lock memory " + std::to_string(other.nbytes) + ", err code " +
                          std::to_string((int32_t)err);
        FAISS_THROW_MSG(msg);
    }
    memcpy(this->data, other.data, other.nbytes);
    this->nbytes = other.nbytes;
}

PageLockMemory::PageLockMemory(PageLockMemory &&other) {
    this->data = other.data;
    this->nbytes = other.nbytes;
    other.data = nullptr;
    other.nbytes = 0;
}

}
#endif

namespace faiss {

InvertedListsIterator::~InvertedListsIterator() {}

/*****************************************
 * InvertedLists implementation
 ******************************************/

InvertedLists::InvertedLists(size_t nlist, size_t code_size)
        : nlist(nlist), code_size(code_size) {}

InvertedLists::~InvertedLists() {}

idx_t InvertedLists::get_single_id(size_t list_no, size_t offset) const {
    assert(offset < list_size(list_no));
    const idx_t* ids = get_ids(list_no);
    idx_t id = ids[offset];
    release_ids(list_no, ids);
    return id;
}

void InvertedLists::release_codes(size_t, const uint8_t*) const {}

void InvertedLists::release_ids(size_t, const idx_t*) const {}

void InvertedLists::prefetch_lists(const idx_t*, int) const {}

const uint8_t* InvertedLists::get_single_code(size_t list_no, size_t offset)
        const {
    assert(offset < list_size(list_no));
    return get_codes(list_no) + offset * code_size;
}

size_t InvertedLists::get_segment_num(size_t list_no) const {
    return 1;
}

size_t InvertedLists::get_segment_size(size_t list_no, size_t segment_no) const {
    return list_size(list_no);
}

size_t InvertedLists::get_segment_offset(size_t list_no, size_t segment_no) const {
    return 0;
}

const uint8_t* InvertedLists::get_codes(size_t list_no, size_t offset)
        const {
    assert(offset < list_size(list_no));
    return get_codes(list_no) + offset * code_size;
}

const idx_t* InvertedLists::get_ids(size_t list_no, size_t offset) const {
    return get_ids(list_no);
}

size_t InvertedLists::add_entry(
        size_t list_no,
        idx_t theid,
        const uint8_t* code,
        const float* code_norm,
        void* /*inverted_list_context*/) {
    return add_entries(list_no, 1, &theid, code, code_norm);
}

void InvertedLists::update_entry(
        size_t list_no,
        size_t offset,
        idx_t id,
        const uint8_t* code) {
    update_entries(list_no, offset, 1, &id, code);
}

InvertedLists* InvertedLists::to_readonly() {
    return nullptr;
}


const float* InvertedLists::get_code_norms(size_t list_no, size_t offset)
        const {
    return nullptr;
}

void InvertedLists::release_code_norms(size_t list_no, const float* codes)
        const {}

bool InvertedLists::is_readonly() const {
    return false;
}

void InvertedLists::reset() {
    for (size_t i = 0; i < nlist; i++) {
        resize(i, 0);
    }
}

void InvertedLists::merge_from(InvertedLists* oivf, size_t add_id) {
#pragma omp parallel for
    for (idx_t i = 0; i < nlist; i++) {
        size_t list_size = oivf->list_size(i);
        ScopedIds ids(oivf, i);
        if (add_id == 0) {
            add_entries(i, list_size, ids.get(), ScopedCodes(oivf, i).get());
        } else {
            std::vector<idx_t> new_ids(list_size);

            for (size_t j = 0; j < list_size; j++) {
                new_ids[j] = ids[j] + add_id;
            }
            add_entries(
                    i, list_size, new_ids.data(), ScopedCodes(oivf, i).get());
        }
        oivf->resize(i, 0);
    }
}

size_t InvertedLists::copy_subset_to(
        InvertedLists& oivf,
        subset_type_t subset_type,
        idx_t a1,
        idx_t a2) const {
    FAISS_THROW_IF_NOT(nlist == oivf.nlist);
    FAISS_THROW_IF_NOT(code_size == oivf.code_size);
    FAISS_THROW_IF_NOT_FMT(
            subset_type >= 0 && subset_type <= 4,
            "subset type %d not implemented",
            subset_type);
    size_t accu_n = 0;
    size_t accu_a1 = 0;
    size_t accu_a2 = 0;
    size_t n_added = 0;

    size_t ntotal = 0;
    if (subset_type == 2) {
        ntotal = compute_ntotal();
    }

    for (idx_t list_no = 0; list_no < nlist; list_no++) {
        size_t n = list_size(list_no);
        ScopedIds ids_in(this, list_no);

        if (subset_type == SUBSET_TYPE_ID_RANGE) {
            for (idx_t i = 0; i < n; i++) {
                idx_t id = ids_in[i];
                if (a1 <= id && id < a2) {
                    oivf.add_entry(
                            list_no,
                            get_single_id(list_no, i),
                            ScopedCodes(this, list_no, i).get());
                    n_added++;
                }
            }
        } else if (subset_type == SUBSET_TYPE_ID_MOD) {
            for (idx_t i = 0; i < n; i++) {
                idx_t id = ids_in[i];
                if (id % a1 == a2) {
                    oivf.add_entry(
                            list_no,
                            get_single_id(list_no, i),
                            ScopedCodes(this, list_no, i).get());
                    n_added++;
                }
            }
        } else if (subset_type == SUBSET_TYPE_ELEMENT_RANGE) {
            // see what is allocated to a1 and to a2
            size_t next_accu_n = accu_n + n;
            size_t next_accu_a1 = next_accu_n * a1 / ntotal;
            size_t i1 = next_accu_a1 - accu_a1;
            size_t next_accu_a2 = next_accu_n * a2 / ntotal;
            size_t i2 = next_accu_a2 - accu_a2;

            for (idx_t i = i1; i < i2; i++) {
                oivf.add_entry(
                        list_no,
                        get_single_id(list_no, i),
                        ScopedCodes(this, list_no, i).get());
            }

            n_added += i2 - i1;
            accu_a1 = next_accu_a1;
            accu_a2 = next_accu_a2;
        } else if (subset_type == SUBSET_TYPE_INVLIST_FRACTION) {
            size_t i1 = n * a2 / a1;
            size_t i2 = n * (a2 + 1) / a1;

            for (idx_t i = i1; i < i2; i++) {
                oivf.add_entry(
                        list_no,
                        get_single_id(list_no, i),
                        ScopedCodes(this, list_no, i).get());
            }

            n_added += i2 - i1;
        } else if (subset_type == SUBSET_TYPE_INVLIST) {
            if (list_no >= a1 && list_no < a2) {
                oivf.add_entries(
                        list_no,
                        n,
                        ScopedIds(this, list_no).get(),
                        ScopedCodes(this, list_no).get());
                n_added += n;
            }
        }
        accu_n += n;
    }
    return n_added;
}

double InvertedLists::imbalance_factor() const {
    std::vector<int> hist(nlist);

    for (size_t i = 0; i < nlist; i++) {
        hist[i] = list_size(i);
    }

    return faiss::imbalance_factor(nlist, hist.data());
}

void InvertedLists::print_stats() const {
    std::vector<int> sizes(40);
    for (size_t i = 0; i < nlist; i++) {
        for (size_t j = 0; j < sizes.size(); j++) {
            if ((list_size(i) >> j) == 0) {
                sizes[j]++;
                break;
            }
        }
    }
    for (size_t i = 0; i < sizes.size(); i++) {
        if (sizes[i]) {
            printf("list size in < %zu: %d instances\n",
                   static_cast<size_t>(1) << i,
                   sizes[i]);
        }
    }
}

size_t InvertedLists::compute_ntotal() const {
    size_t tot = 0;
    for (size_t i = 0; i < nlist; i++) {
        tot += list_size(i);
    }
    return tot;
}

bool InvertedLists::is_empty(size_t list_no, void* inverted_list_context)
        const {
    if (use_iterator) {
        return !std::unique_ptr<InvertedListsIterator>(
                        get_iterator(list_no, inverted_list_context))
                        ->is_available();
    } else {
        FAISS_THROW_IF_NOT(inverted_list_context == nullptr);
        return list_size(list_no) == 0;
    }
}

// implemnent iterator on top of get_codes / get_ids
namespace {

struct CodeArrayIterator : InvertedListsIterator {
    size_t list_size;
    size_t code_size;
    InvertedLists::ScopedCodes codes;
    InvertedLists::ScopedIds ids;
    size_t idx = 0;

    CodeArrayIterator(const InvertedLists* il, size_t list_no)
            : list_size(il->list_size(list_no)),
              code_size(il->code_size),
              codes(il, list_no),
              ids(il, list_no) {}

    bool is_available() const override {
        return idx < list_size;
    }
    void next() override {
        idx++;
    }
    std::pair<idx_t, const uint8_t*> get_id_and_codes() override {
        return {ids[idx], codes.get() + code_size * idx};
    }
};

} // namespace

InvertedListsIterator* InvertedLists::get_iterator(
        size_t list_no,
        void* inverted_list_context) const {
    FAISS_THROW_IF_NOT(inverted_list_context == nullptr);
    return new CodeArrayIterator(this, list_no);
}

/*****************************************
 * ArrayInvertedLists implementation
 ******************************************/

ArrayInvertedLists::ArrayInvertedLists(
        size_t nlist,
        size_t code_size,
        bool _with_norm)
        : with_norm(_with_norm), InvertedLists(nlist, code_size) {
    ids.resize(nlist);
    codes.resize(nlist);
    if (with_norm) {
        code_norms.resize(nlist);
    }
}

size_t ArrayInvertedLists::add_entries(
        size_t list_no,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* code,
        const float* code_norms_in) {
    if (n_entry == 0)
        return 0;
    assert(list_no < nlist);
    size_t o = ids[list_no].size();
    ids[list_no].resize(o + n_entry);
    memcpy(&ids[list_no][o], ids_in, sizeof(ids_in[0]) * n_entry);
    codes[list_no].resize((o + n_entry) * code_size);
    memcpy(&codes[list_no][o * code_size], code, code_size * n_entry);
    if (with_norm) {
        code_norms[list_no].resize(o + n_entry);
        memcpy(&code_norms[list_no][o], code_norms_in, sizeof(float) * n_entry);
    }
    return o;
}

size_t ArrayInvertedLists::list_size(size_t list_no) const {
    assert(list_no < nlist);
    return ids[list_no].size();
}

bool ArrayInvertedLists::is_empty(size_t list_no, void* inverted_list_context)
        const {
    FAISS_THROW_IF_NOT(inverted_list_context == nullptr);
    return ids[list_no].size() == 0;
}

const uint8_t* ArrayInvertedLists::get_codes(size_t list_no) const {
    assert(list_no < nlist);
    return codes[list_no].data();
}

const idx_t* ArrayInvertedLists::get_ids(size_t list_no) const {
    assert(list_no < nlist);
    return ids[list_no].data();
}

void ArrayInvertedLists::resize(size_t list_no, size_t new_size) {
    ids[list_no].resize(new_size);
    codes[list_no].resize(new_size * code_size);
}

// temp code for IVF_FLAT_NM backward compatibility
void ArrayInvertedLists::restore_codes(
        const uint8_t* raw_data,
        const size_t raw_size,
        const bool is_cosine) {
    size_t total = 0;
    with_norm = is_cosine;
    codes.resize(nlist);
    if (is_cosine) {
        code_norms.resize(nlist);
    }
    for (size_t i = 0; i < nlist; i++) {
        auto list_size = ids[i].size();
        total += list_size;
        codes[i].resize(list_size * code_size);
        if (is_cosine) {
            code_norms[i].resize(list_size);
        }
        uint8_t* dst = codes[i].data();
        for (size_t j = 0; j < list_size; j++) {
            const uint8_t* src = raw_data + code_size * ids[i][j];
            std::copy_n(src, code_size, dst);
            dst += code_size;
        }
        if (is_cosine) {
            fvec_norms_L2(code_norms[i].data(),
                          (const float*)codes[i].data(),
                          code_size / sizeof(float),
                          list_size);
        }
    }
    assert(total * code_size == raw_size);
}

const float* ArrayInvertedLists::get_code_norms(
        size_t list_no,
        size_t offset) const {
    if (with_norm) {
        assert(list_no < nlist);
        return code_norms[list_no].data();
    } else {
        return nullptr;
    }
}

void ArrayInvertedLists::release_code_norms(
        size_t list_no,
        const float* codes) const {
    InvertedLists::release_code_norms(list_no, codes);
}

void ArrayInvertedLists::update_entries(
        size_t list_no,
        size_t offset,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* codes_in) {
    assert(list_no < nlist);
    assert(n_entry + offset <= ids[list_no].size());
    memcpy(&ids[list_no][offset], ids_in, sizeof(ids_in[0]) * n_entry);
    memcpy(&codes[list_no][offset * code_size], codes_in, code_size * n_entry);
}

InvertedLists* ArrayInvertedLists::to_readonly() {
    return new ReadOnlyArrayInvertedLists(*this);
}

void ArrayInvertedLists::permute_invlists(const idx_t* map) {
    // todo aguzhva: permute norms as well?
    std::vector<MaybeOwnedVector<uint8_t>> new_codes(nlist);
    std::vector<MaybeOwnedVector<idx_t>> new_ids(nlist);

    for (size_t i = 0; i < nlist; i++) {
        size_t o = map[i];
        FAISS_THROW_IF_NOT(o < nlist);
        std::swap(new_codes[i], codes[o]);
        std::swap(new_ids[i], ids[o]);
    }
    std::swap(codes, new_codes);
    std::swap(ids, new_ids);
}

ArrayInvertedLists::~ArrayInvertedLists() {}

ConcurrentArrayInvertedLists::ConcurrentArrayInvertedLists(
        size_t nlist,
        size_t code_size,
        size_t segment_size,
        bool snorm)
        : InvertedLists(nlist, code_size), segment_size(segment_size), save_norm(snorm), list_cur(nlist) {
    ids.resize(nlist);
    if (save_norm) {
        code_norms.resize(nlist);
    }
    codes.resize(nlist);
    for (int i = 0; i < nlist; i++) {
        list_cur[i].store(0);
    }
}

size_t ConcurrentArrayInvertedLists::cal_segment_num(size_t capacity) const {
    return (capacity / segment_size) + (capacity % segment_size != 0);
}

void ConcurrentArrayInvertedLists::reserve(size_t list_no, size_t capacity) {
    size_t cur_segment_no = ids[list_no].size();
    size_t target_segment_no = cal_segment_num(capacity);

    for (size_t idx = cur_segment_no; idx < target_segment_no; idx++) {
        Segment<uint8_t> segment_codes(segment_size, code_size);
        Segment<idx_t> segment_ids(segment_size, 1);
        Segment<float> segment_code_norms(segment_size, 1);
        codes[list_no].emplace_back(std::move(segment_codes));
        if (save_norm) {
            code_norms[list_no].emplace_back(std::move(segment_code_norms));
        }
        ids[list_no].emplace_back(std::move(segment_ids));
    }
}

void ConcurrentArrayInvertedLists::shrink_to_fit(size_t list_no, size_t capacity) {
    size_t cur_segment_no = ids[list_no].size();
    size_t target_segment_no = cal_segment_num(capacity);

    for (size_t idx = cur_segment_no; idx > target_segment_no; idx--) {
        ids[list_no].pop_back();
        if (save_norm) {
            code_norms[list_no].pop_back();
        }
        codes[list_no].pop_back();
    }
}

size_t ConcurrentArrayInvertedLists::list_size(size_t list_no) const {
    assert(list_no < nlist);
    return list_cur[list_no].load();
}

const uint8_t* ConcurrentArrayInvertedLists::get_codes(size_t list_no) const {
    FAISS_THROW_MSG("not implemented get_codes for non-continuous storage");
}

const idx_t* ConcurrentArrayInvertedLists::get_ids(size_t list_no) const {
    FAISS_THROW_MSG("not implemented get_ids for non-continuous storage");
}
size_t ConcurrentArrayInvertedLists::add_entries(
        size_t list_no,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* codes_in,
        const float* code_norms_in) {
    if (n_entry == 0)
        return 0;

    assert(list_no < nlist);
    size_t o = list_size(list_no);

    reserve(list_no, o + n_entry);

    int64_t first_id  = o / segment_size;
    int64_t first_cur = o % segment_size;

    if (first_cur + n_entry <= segment_size) {
        std::memcpy(&ids[list_no][first_id][first_cur], ids_in, n_entry * sizeof(ids_in[0]));
        if (save_norm) {
            std::memcpy(
                    &code_norms[list_no][first_id][first_cur],
                    code_norms_in,
                    n_entry * sizeof(float));
        }
        std::memcpy(&codes[list_no][first_id][first_cur], codes_in, n_entry * code_size);
        list_cur[list_no].fetch_add(n_entry);
        return o;
    }

    //process first segment
    int64_t first_rest = segment_size - first_cur;
    memcpy(&ids[list_no][first_id][first_cur],
           ids_in,
           first_rest * sizeof(ids_in[0]));
    if (save_norm) {
        memcpy(&code_norms[list_no][first_id][first_cur],
           code_norms_in,
           first_rest * sizeof(float));
    }
    memcpy(&codes[list_no][first_id][first_cur],
           codes_in,
           first_rest * code_size);
    list_cur[list_no].fetch_add(first_rest);

    auto rest_entry = n_entry - first_rest;
    auto entry_cur  = first_rest;
    auto segment_id = first_id;
    //process rest segment
    while (rest_entry > 0) {
        segment_id = segment_id + 1;
        if (rest_entry >= segment_size) {
            memcpy(&codes[list_no][segment_id][0],
                   codes_in + entry_cur * code_size,
                   segment_size * code_size);
            if (save_norm) {
                memcpy(&code_norms[list_no][segment_id][0],
                       code_norms_in + entry_cur,
                       segment_size * sizeof(code_norms_in[0]));
            }
            memcpy(&ids[list_no][segment_id][0],
                   ids_in + entry_cur,
                   segment_size * sizeof(ids_in[0]));
            list_cur[list_no].fetch_add(segment_size);

            entry_cur += segment_size;
            rest_entry -= segment_size;
        } else {
            memcpy(&codes[list_no][segment_id][0],
                   codes_in + entry_cur * code_size,
                   rest_entry * code_size);
            if (save_norm) {
                memcpy(&code_norms[list_no][segment_id][0],
                       code_norms_in + entry_cur,
                       rest_entry * sizeof(float));
            }
            memcpy(&ids[list_no][segment_id][0],
                   ids_in + entry_cur,
                   rest_entry * sizeof(ids_in[0]));
            list_cur[list_no].fetch_add(rest_entry);

            entry_cur += rest_entry;
            rest_entry -= rest_entry;
        }
    }
    assert(entry_cur == n_entry);
    assert(rest_entry == 0);
    return o;
}

//Not used
void ConcurrentArrayInvertedLists::update_entries(
        size_t list_no,
        size_t offset,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* codes_in) {
    if (n_entry == 0)
        return;

    assert(list_no < nlist);
    assert(n_entry + offset <= list_size(list_no));

    size_t  first_id  = offset / segment_size;
    int64_t first_cur = offset % segment_size;
    int64_t first_rest = segment_size - first_cur;

    if (n_entry <= first_rest) {
        memcpy(&ids[list_no][first_id][first_cur], ids_in, n_entry * sizeof(ids_in[0]));
        memcpy(&codes[list_no][first_id][first_cur], codes_in, n_entry * code_size);
        return ;
    }

    // update first segment
    memcpy(&ids[list_no][first_id][first_cur],
           ids_in,
           first_rest * sizeof(ids_in[0]));
    memcpy(&codes[list_no][first_id][first_cur],
           codes_in,
           first_rest * code_size);

    auto rest_entry = n_entry - first_rest;
    auto entry_cur = first_rest;
    auto segment_id = first_id;
    while (rest_entry > 0) {
        segment_id = segment_id + 1;
        if (rest_entry >= segment_size) {
            memcpy(&codes[list_no][segment_id][0],
                   codes_in + entry_cur * code_size,
                   segment_size * code_size);
            memcpy(&ids[list_no][segment_id][0],
                   ids_in + entry_cur,
                   segment_size * sizeof(ids_in[0]));

            entry_cur += segment_size;
            rest_entry -= segment_size;
        } else {
            memcpy(&codes[list_no][segment_id][0],
                   codes_in + entry_cur * code_size,
                   rest_entry * code_size);
            memcpy(&ids[list_no][segment_id][0],
                   ids_in + entry_cur,
                   rest_entry * sizeof(ids_in[0]));

            entry_cur += rest_entry;
            rest_entry -= rest_entry;
        }
    }
    assert(entry_cur == n_entry);
    assert(rest_entry == 0);
}

InvertedLists* ConcurrentArrayInvertedLists::to_readonly() {
    return InvertedLists::to_readonly();
}

ConcurrentArrayInvertedLists::~ConcurrentArrayInvertedLists() {
}

void ConcurrentArrayInvertedLists::resize(size_t list_no, size_t new_size) {
    size_t o = list_size(list_no);

    if (new_size >= o) {
        reserve(list_no, new_size);
        list_cur[list_no].store(new_size);
    } else {
        list_cur[list_no].store(new_size);
        shrink_to_fit(list_no, new_size);
    }

}
size_t ConcurrentArrayInvertedLists::get_segment_num(size_t list_no) const {
    assert(list_no < nlist);
    auto o = list_cur[list_no].load();
    return (o / segment_size) + (o % segment_size != 0);
}
size_t ConcurrentArrayInvertedLists::get_segment_size(
        size_t list_no,
        size_t segment_no) const {
    assert(list_no < nlist);
    auto o = list_cur[list_no].load();
    if (segment_no == 0 && o == 0) {
        return 0;
    }
    auto seg_o = cal_segment_num(o);
    assert(segment_no < seg_o);
    auto o_last = o % segment_size > 0 ? o % segment_size : segment_size;
    if (segment_no < seg_o - 1) {
        return segment_size;
    } else {
        return o_last;
    }
}
size_t ConcurrentArrayInvertedLists::get_segment_offset(
        size_t list_no,
        size_t segment_no) const {
    assert(list_no < nlist);
    auto o = list_cur[list_no].load();
    auto seg_o = cal_segment_num(o);
    assert(segment_no < seg_o);
    return segment_size * segment_no;
}
const uint8_t* ConcurrentArrayInvertedLists::get_codes(
        size_t list_no,
        size_t offset) const {
    assert(list_no < nlist);
    assert(offset < list_size(list_no));
    auto segment_no = offset / segment_size;
    auto segment_off = offset % segment_size;
    return reinterpret_cast<const uint8_t *>(&(codes[list_no][segment_no][segment_off]));
}

const idx_t* ConcurrentArrayInvertedLists::get_ids(
        size_t list_no,
        size_t offset) const {
    assert(list_no < nlist);
    assert(offset < list_size(list_no));
    auto segment_no = offset / segment_size;
    auto segment_off = offset % segment_size;
    return reinterpret_cast<const idx_t *>(&(ids[list_no][segment_no][segment_off]));
}


const uint8_t* ConcurrentArrayInvertedLists::get_single_code(
        size_t list_no,
        size_t offset) const {
    return get_codes(list_no, offset);
}

idx_t ConcurrentArrayInvertedLists::get_single_id(size_t list_no, size_t offset)
        const {
    auto *pItem = get_ids(list_no, offset);
    return *pItem;
}

const float* ConcurrentArrayInvertedLists::get_code_norms(
        size_t list_no,
        size_t offset) const {
    if (!save_norm) {
        return nullptr;
    } else {
        assert(list_no < nlist);
        assert(offset < list_size(list_no));
        auto segment_no = offset / segment_size;
        auto segment_off = offset % segment_size;
        return &(code_norms[list_no][segment_no][segment_off]);
    }
}
void ConcurrentArrayInvertedLists::release_code_norms(
        size_t list_no,
        const float* codes) const {
    InvertedLists::release_code_norms(list_no, codes);
}

/*****************************************************************
 * ReadOnlyArrayInvertedLists implementations
 *****************************************************************/

ReadOnlyArrayInvertedLists::ReadOnlyArrayInvertedLists(
        size_t nlist,
        size_t code_size,
        const std::vector<size_t>& list_length)
        : InvertedLists (nlist, code_size),
          readonly_length(list_length) {
    valid = readonly_length.size() == nlist;
    if (!valid) {
        FAISS_THROW_MSG("Invalid list_length");
    }
    auto total_size = std::accumulate(readonly_length.begin(), readonly_length.end(), 0);
    readonly_offset.reserve(nlist);

#ifndef USE_GPU
    readonly_codes.reserve(total_size * code_size);
    readonly_ids.reserve(total_size);
#endif

    size_t offset = 0;
    for (auto i=0; i<readonly_length.size(); ++i) {
        readonly_offset.emplace_back(offset);
        offset += readonly_length[i];
    }
}

ReadOnlyArrayInvertedLists::ReadOnlyArrayInvertedLists(
        const ArrayInvertedLists& other)
        : InvertedLists (other.nlist, other.code_size) {
    readonly_length.resize(nlist);
    readonly_offset.resize(nlist);
    size_t offset = 0;
    for (auto i = 0; i < other.ids.size(); i++) {
        auto& list_ids = other.ids[i];
        readonly_length[i] = list_ids.size();
        readonly_offset[i] = offset;
        offset += list_ids.size();
    }

#ifdef USE_GPU
    size_t ids_size = offset * sizeof(idx_t);
    size_t codes_size = offset * (this->code_size) * sizeof(uint8_t);
    pin_readonly_codes = std::make_shared<PageLockMemory>(codes_size);
    pin_readonly_ids = std::make_shared<PageLockMemory>(ids_size);

    offset = 0;
    for (auto i = 0; i < other.ids.size(); i++) {
        auto& list_ids = other.ids[i];
        auto& list_codes = other.codes[i];

        uint8_t* ids_ptr = (uint8_t*)(pin_readonly_ids->data) + offset * sizeof(idx_t);
        memcpy(ids_ptr, list_ids.data(), list_ids.size() * sizeof(idx_t));

        uint8_t* codes_ptr = (uint8_t*)(pin_readonly_codes->data) + offset * (this->code_size) * sizeof(uint8_t);
        memcpy(codes_ptr, list_codes.data(), list_codes.size() * sizeof(uint8_t));

        offset += list_ids.size();
    }
#else
    for (auto i = 0; i < other.ids.size(); i++) {
        auto& list_ids = other.ids[i];
        readonly_ids.insert(readonly_ids.end(), list_ids.begin(), list_ids.end());

        auto& list_codes = other.codes[i];
        readonly_codes.insert(readonly_codes.end(), list_codes.begin(), list_codes.end());
    }
#endif

    valid = true;
}

ReadOnlyArrayInvertedLists::ReadOnlyArrayInvertedLists(
        const ArrayInvertedLists& other,
        bool offset_only)
        : InvertedLists (other.nlist, other.code_size) {
    readonly_length.resize(nlist);
    readonly_offset.resize(nlist);
    size_t offset = 0;
    for (auto i = 0; i < other.ids.size(); i++) {
        auto& list_ids = other.ids[i];
        readonly_length[i] = list_ids.size();
        readonly_offset[i] = offset;
        offset += list_ids.size();
    }

#ifdef USE_GPU
    size_t ids_size = offset * sizeof(idx_t);
    size_t codes_size = offset * (this->code_size) * sizeof(uint8_t);
    pin_readonly_codes = std::make_shared<PageLockMemory>(codes_size);
    pin_readonly_ids = std::make_shared<PageLockMemory>(ids_size);

    offset = 0;
    for (auto i = 0; i < other.ids.size(); i++) {
        auto& list_ids = other.ids[i];

        uint8_t* ids_ptr = (uint8_t*)(pin_readonly_ids->data) + offset * sizeof(idx_t);
        memcpy(ids_ptr, list_ids.data(), list_ids.size() * sizeof(idx_t));

        offset += list_ids.size();
    }
#else
    for (auto i = 0; i < other.ids.size(); i++) {
        auto& list_ids = other.ids[i];
        readonly_ids.insert(readonly_ids.end(), list_ids.begin(), list_ids.end());
    }
#endif

    valid = true;
}

ReadOnlyArrayInvertedLists::~ReadOnlyArrayInvertedLists() {
}

bool
ReadOnlyArrayInvertedLists::is_valid() {
    return valid;
}

size_t ReadOnlyArrayInvertedLists::add_entries(
        size_t,
        size_t,
        const idx_t*,
        const uint8_t*,
        const float* code_norm) {
    FAISS_THROW_MSG("not implemented");
}

void ReadOnlyArrayInvertedLists::update_entries(
        size_t,
        size_t,
        size_t,
        const idx_t*,
        const uint8_t*) {
    FAISS_THROW_MSG("not implemented");
}

void ReadOnlyArrayInvertedLists::resize(
        size_t,
        size_t) {
    FAISS_THROW_MSG("not implemented");
}

size_t ReadOnlyArrayInvertedLists::list_size(
        size_t list_no) const {
    FAISS_ASSERT(list_no < nlist && valid);
    return readonly_length[list_no];
}

const uint8_t* ReadOnlyArrayInvertedLists::get_codes(
        size_t list_no) const {
    FAISS_ASSERT(list_no < nlist && valid);
#ifdef USE_GPU
    uint8_t* pcodes = (uint8_t*)(pin_readonly_codes->data);
    return pcodes + readonly_offset[list_no] * code_size;
#else
    return readonly_codes.data() + readonly_offset[list_no] * code_size;
#endif
}

const idx_t* ReadOnlyArrayInvertedLists::get_ids(
        size_t list_no) const {
    FAISS_ASSERT(list_no < nlist && valid);
#ifdef USE_GPU
    idx_t* pids = (idx_t*)pin_readonly_ids->data;
    return pids + readonly_offset[list_no];
#else
    return readonly_ids.data() + readonly_offset[list_no];
#endif
}

const idx_t* ReadOnlyArrayInvertedLists::get_all_ids() const {
    FAISS_ASSERT(valid);
#ifdef USE_GPU
    return (idx_t*)(pin_readonly_ids->data);
#else
    return readonly_ids.data();
#endif
}

const uint8_t* ReadOnlyArrayInvertedLists::get_all_codes() const {
    FAISS_ASSERT(valid);
#ifdef USE_GPU
    return (uint8_t*)(pin_readonly_codes->data);
#else
    return readonly_codes.data();
#endif
}

const std::vector<size_t>& ReadOnlyArrayInvertedLists::get_list_length() const {
    FAISS_ASSERT(valid);
    return readonly_length;
}

bool ReadOnlyArrayInvertedLists::is_readonly() const {
    FAISS_ASSERT(valid);
    return true;
}

/*****************************************************************
 * Meta-inverted list implementations
 *****************************************************************/

size_t ReadOnlyInvertedLists::add_entries(
        size_t,
        size_t,
        const idx_t*,
        const uint8_t*,
        const float*) {
    FAISS_THROW_MSG("not implemented");
}

void ReadOnlyInvertedLists::update_entries(
        size_t,
        size_t,
        size_t,
        const idx_t*,
        const uint8_t*) {
    FAISS_THROW_MSG("not implemented");
}

void ReadOnlyInvertedLists::resize(size_t, size_t) {
    FAISS_THROW_MSG("not implemented");
}

/*****************************************
 * HStackInvertedLists implementation
 ******************************************/

HStackInvertedLists::HStackInvertedLists(int nil, const InvertedLists** ils_in)
        : ReadOnlyInvertedLists(
                  nil > 0 ? ils_in[0]->nlist : 0,
                  nil > 0 ? ils_in[0]->code_size : 0) {
    FAISS_THROW_IF_NOT(nil > 0);
    for (int i = 0; i < nil; i++) {
        ils.push_back(ils_in[i]);
        FAISS_THROW_IF_NOT(
                ils_in[i]->code_size == code_size && ils_in[i]->nlist == nlist);
    }
}

size_t HStackInvertedLists::list_size(size_t list_no) const {
    size_t sz = 0;
    for (int i = 0; i < ils.size(); i++) {
        const InvertedLists* il = ils[i];
        sz += il->list_size(list_no);
    }
    return sz;
}

const uint8_t* HStackInvertedLists::get_codes(size_t list_no) const {
    uint8_t *codes = new uint8_t[code_size * list_size(list_no)], *c = codes;

    for (int i = 0; i < ils.size(); i++) {
        const InvertedLists* il = ils[i];
        size_t sz = il->list_size(list_no) * code_size;
        if (sz > 0) {
            memcpy(c, ScopedCodes(il, list_no).get(), sz);
            c += sz;
        }
    }
    return codes;
}

const uint8_t* HStackInvertedLists::get_single_code(
        size_t list_no,
        size_t offset) const {
    for (int i = 0; i < ils.size(); i++) {
        const InvertedLists* il = ils[i];
        size_t sz = il->list_size(list_no);
        if (offset < sz) {
            // here we have to copy the code, otherwise it will crash at dealloc
            uint8_t* code = new uint8_t[code_size];
            memcpy(code, ScopedCodes(il, list_no, offset).get(), code_size);
            return code;
        }
        offset -= sz;
    }
    FAISS_THROW_FMT("offset %zd unknown", offset);
}

void HStackInvertedLists::release_codes(size_t, const uint8_t* codes) const {
    delete[] codes;
}

const idx_t* HStackInvertedLists::get_ids(size_t list_no) const {
    idx_t *ids = new idx_t[list_size(list_no)], *c = ids;

    for (int i = 0; i < ils.size(); i++) {
        const InvertedLists* il = ils[i];
        size_t sz = il->list_size(list_no);
        if (sz > 0) {
            memcpy(c, ScopedIds(il, list_no).get(), sz * sizeof(idx_t));
            c += sz;
        }
    }
    return ids;
}

idx_t HStackInvertedLists::get_single_id(size_t list_no, size_t offset) const {
    for (int i = 0; i < ils.size(); i++) {
        const InvertedLists* il = ils[i];
        size_t sz = il->list_size(list_no);
        if (offset < sz) {
            return il->get_single_id(list_no, offset);
        }
        offset -= sz;
    }
    FAISS_THROW_FMT("offset %zd unknown", offset);
}

void HStackInvertedLists::release_ids(size_t, const idx_t* ids) const {
    delete[] ids;
}

void HStackInvertedLists::prefetch_lists(const idx_t* list_nos, int nlist)
        const {
    for (int i = 0; i < ils.size(); i++) {
        const InvertedLists* il = ils[i];
        il->prefetch_lists(list_nos, nlist);
    }
}

/*****************************************
 * SliceInvertedLists implementation
 ******************************************/

namespace {

idx_t translate_list_no(const SliceInvertedLists* sil, idx_t list_no) {
    FAISS_THROW_IF_NOT(list_no >= 0 && list_no < sil->nlist);
    return list_no + sil->i0;
}

} // namespace

SliceInvertedLists::SliceInvertedLists(
        const InvertedLists* il,
        idx_t i0,
        idx_t i1)
        : ReadOnlyInvertedLists(i1 - i0, il->code_size),
          il(il),
          i0(i0),
          i1(i1) {}

size_t SliceInvertedLists::list_size(size_t list_no) const {
    return il->list_size(translate_list_no(this, list_no));
}

const uint8_t* SliceInvertedLists::get_codes(size_t list_no) const {
    return il->get_codes(translate_list_no(this, list_no));
}

const uint8_t* SliceInvertedLists::get_single_code(
        size_t list_no,
        size_t offset) const {
    return il->get_single_code(translate_list_no(this, list_no), offset);
}

void SliceInvertedLists::release_codes(size_t list_no, const uint8_t* codes)
        const {
    return il->release_codes(translate_list_no(this, list_no), codes);
}

const idx_t* SliceInvertedLists::get_ids(size_t list_no) const {
    return il->get_ids(translate_list_no(this, list_no));
}

idx_t SliceInvertedLists::get_single_id(size_t list_no, size_t offset) const {
    return il->get_single_id(translate_list_no(this, list_no), offset);
}

void SliceInvertedLists::release_ids(size_t list_no, const idx_t* ids) const {
    return il->release_ids(translate_list_no(this, list_no), ids);
}

void SliceInvertedLists::prefetch_lists(const idx_t* list_nos, int nlist)
        const {
    std::vector<idx_t> translated_list_nos;
    for (int j = 0; j < nlist; j++) {
        idx_t list_no = list_nos[j];
        if (list_no < 0)
            continue;
        translated_list_nos.push_back(translate_list_no(this, list_no));
    }
    il->prefetch_lists(translated_list_nos.data(), translated_list_nos.size());
}

/*****************************************
 * VStackInvertedLists implementation
 ******************************************/

namespace {

// find the invlist this number belongs to
int translate_list_no(const VStackInvertedLists* vil, idx_t list_no) {
    FAISS_THROW_IF_NOT(list_no >= 0 && list_no < vil->nlist);
    int i0 = 0, i1 = vil->ils.size();
    const idx_t* cumsz = vil->cumsz.data();
    while (i0 + 1 < i1) {
        int imed = (i0 + i1) / 2;
        if (list_no >= cumsz[imed]) {
            i0 = imed;
        } else {
            i1 = imed;
        }
    }
    assert(list_no >= cumsz[i0] && list_no < cumsz[i0 + 1]);
    return i0;
}

idx_t sum_il_sizes(int nil, const InvertedLists** ils_in) {
    idx_t tot = 0;
    for (int i = 0; i < nil; i++) {
        tot += ils_in[i]->nlist;
    }
    return tot;
}

} // namespace

VStackInvertedLists::VStackInvertedLists(int nil, const InvertedLists** ils_in)
        : ReadOnlyInvertedLists(
                  sum_il_sizes(nil, ils_in),
                  nil > 0 ? ils_in[0]->code_size : 0) {
    FAISS_THROW_IF_NOT(nil > 0);
    cumsz.resize(nil + 1);
    for (int i = 0; i < nil; i++) {
        ils.push_back(ils_in[i]);
        FAISS_THROW_IF_NOT(ils_in[i]->code_size == code_size);
        cumsz[i + 1] = cumsz[i] + ils_in[i]->nlist;
    }
}

size_t VStackInvertedLists::list_size(size_t list_no) const {
    int i = translate_list_no(this, list_no);
    list_no -= cumsz[i];
    return ils[i]->list_size(list_no);
}

const uint8_t* VStackInvertedLists::get_codes(size_t list_no) const {
    int i = translate_list_no(this, list_no);
    list_no -= cumsz[i];
    return ils[i]->get_codes(list_no);
}

const uint8_t* VStackInvertedLists::get_single_code(
        size_t list_no,
        size_t offset) const {
    int i = translate_list_no(this, list_no);
    list_no -= cumsz[i];
    return ils[i]->get_single_code(list_no, offset);
}

void VStackInvertedLists::release_codes(size_t list_no, const uint8_t* codes)
        const {
    int i = translate_list_no(this, list_no);
    list_no -= cumsz[i];
    return ils[i]->release_codes(list_no, codes);
}

const idx_t* VStackInvertedLists::get_ids(size_t list_no) const {
    int i = translate_list_no(this, list_no);
    list_no -= cumsz[i];
    return ils[i]->get_ids(list_no);
}

idx_t VStackInvertedLists::get_single_id(size_t list_no, size_t offset) const {
    int i = translate_list_no(this, list_no);
    list_no -= cumsz[i];
    return ils[i]->get_single_id(list_no, offset);
}

void VStackInvertedLists::release_ids(size_t list_no, const idx_t* ids) const {
    int i = translate_list_no(this, list_no);
    list_no -= cumsz[i];
    return ils[i]->release_ids(list_no, ids);
}

void VStackInvertedLists::prefetch_lists(const idx_t* list_nos, int nlist)
        const {
    std::vector<int> ilno(nlist, -1);
    std::vector<int> n_per_il(ils.size(), 0);
    for (int j = 0; j < nlist; j++) {
        idx_t list_no = list_nos[j];
        if (list_no < 0)
            continue;
        int i = ilno[j] = translate_list_no(this, list_no);
        n_per_il[i]++;
    }
    std::vector<int> cum_n_per_il(ils.size() + 1, 0);
    for (int j = 0; j < ils.size(); j++) {
        cum_n_per_il[j + 1] = cum_n_per_il[j] + n_per_il[j];
    }
    std::vector<idx_t> sorted_list_nos(cum_n_per_il.back());
    for (int j = 0; j < nlist; j++) {
        idx_t list_no = list_nos[j];
        if (list_no < 0)
            continue;
        int i = ilno[j];
        list_no -= cumsz[i];
        sorted_list_nos[cum_n_per_il[i]++] = list_no;
    }

    int i0 = 0;
    for (int j = 0; j < ils.size(); j++) {
        int i1 = i0 + n_per_il[j];
        if (i1 > i0) {
            ils[j]->prefetch_lists(sorted_list_nos.data() + i0, i1 - i0);
        }
        i0 = i1;
    }
}

/*****************************************
 * MaskedInvertedLists implementation
 ******************************************/

MaskedInvertedLists::MaskedInvertedLists(
        const InvertedLists* il0,
        const InvertedLists* il1)
        : ReadOnlyInvertedLists(il0->nlist, il0->code_size),
          il0(il0),
          il1(il1) {
    FAISS_THROW_IF_NOT(il1->nlist == nlist);
    FAISS_THROW_IF_NOT(il1->code_size == code_size);
}

size_t MaskedInvertedLists::list_size(size_t list_no) const {
    size_t sz = il0->list_size(list_no);
    return sz ? sz : il1->list_size(list_no);
}

const uint8_t* MaskedInvertedLists::get_codes(size_t list_no) const {
    size_t sz = il0->list_size(list_no);
    return (sz ? il0 : il1)->get_codes(list_no);
}

const idx_t* MaskedInvertedLists::get_ids(size_t list_no) const {
    size_t sz = il0->list_size(list_no);
    return (sz ? il0 : il1)->get_ids(list_no);
}

void MaskedInvertedLists::release_codes(size_t list_no, const uint8_t* codes)
        const {
    size_t sz = il0->list_size(list_no);
    (sz ? il0 : il1)->release_codes(list_no, codes);
}

void MaskedInvertedLists::release_ids(size_t list_no, const idx_t* ids) const {
    size_t sz = il0->list_size(list_no);
    (sz ? il0 : il1)->release_ids(list_no, ids);
}

idx_t MaskedInvertedLists::get_single_id(size_t list_no, size_t offset) const {
    size_t sz = il0->list_size(list_no);
    return (sz ? il0 : il1)->get_single_id(list_no, offset);
}

const uint8_t* MaskedInvertedLists::get_single_code(
        size_t list_no,
        size_t offset) const {
    size_t sz = il0->list_size(list_no);
    return (sz ? il0 : il1)->get_single_code(list_no, offset);
}

void MaskedInvertedLists::prefetch_lists(const idx_t* list_nos, int nlist)
        const {
    std::vector<idx_t> list0, list1;
    for (int i = 0; i < nlist; i++) {
        idx_t list_no = list_nos[i];
        if (list_no < 0)
            continue;
        size_t sz = il0->list_size(list_no);
        (sz ? list0 : list1).push_back(list_no);
    }
    il0->prefetch_lists(list0.data(), list0.size());
    il1->prefetch_lists(list1.data(), list1.size());
}

/*****************************************
 * MaskedInvertedLists implementation
 ******************************************/

StopWordsInvertedLists::StopWordsInvertedLists(
        const InvertedLists* il0,
        size_t maxsize)
        : ReadOnlyInvertedLists(il0->nlist, il0->code_size),
          il0(il0),
          maxsize(maxsize) {}

size_t StopWordsInvertedLists::list_size(size_t list_no) const {
    size_t sz = il0->list_size(list_no);
    return sz < maxsize ? sz : 0;
}

const uint8_t* StopWordsInvertedLists::get_codes(size_t list_no) const {
    return il0->list_size(list_no) < maxsize ? il0->get_codes(list_no)
                                             : nullptr;
}

const idx_t* StopWordsInvertedLists::get_ids(size_t list_no) const {
    return il0->list_size(list_no) < maxsize ? il0->get_ids(list_no) : nullptr;
}

void StopWordsInvertedLists::release_codes(size_t list_no, const uint8_t* codes)
        const {
    if (il0->list_size(list_no) < maxsize) {
        il0->release_codes(list_no, codes);
    }
}

void StopWordsInvertedLists::release_ids(size_t list_no, const idx_t* ids)
        const {
    if (il0->list_size(list_no) < maxsize) {
        il0->release_ids(list_no, ids);
    }
}

idx_t StopWordsInvertedLists::get_single_id(size_t list_no, size_t offset)
        const {
    FAISS_THROW_IF_NOT(il0->list_size(list_no) < maxsize);
    return il0->get_single_id(list_no, offset);
}

const uint8_t* StopWordsInvertedLists::get_single_code(
        size_t list_no,
        size_t offset) const {
    FAISS_THROW_IF_NOT(il0->list_size(list_no) < maxsize);
    return il0->get_single_code(list_no, offset);
}

void StopWordsInvertedLists::prefetch_lists(const idx_t* list_nos, int nlist)
        const {
    std::vector<idx_t> list0;
    for (int i = 0; i < nlist; i++) {
        idx_t list_no = list_nos[i];
        if (list_no < 0)
            continue;
        if (il0->list_size(list_no) < maxsize) {
            list0.push_back(list_no);
        }
    }
    il0->prefetch_lists(list0.data(), list0.size());
}

} // namespace faiss
