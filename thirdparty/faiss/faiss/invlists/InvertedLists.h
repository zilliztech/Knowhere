/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#ifndef FAISS_INVERTEDLISTS_IVF_H
#define FAISS_INVERTEDLISTS_IVF_H

/**
 * Definition of inverted lists + a few common classes that implement
 * the interface.
 */

#include <atomic>
#include <cassert>
#include <deque>
#include <memory>
#include <set>
#include <vector>

#include <faiss/MetricType.h>

namespace faiss {

struct PageLockMemory {
public:
    PageLockMemory() : data(nullptr), nbytes(0) {}

    PageLockMemory(size_t size);

    ~PageLockMemory();

    PageLockMemory(const PageLockMemory& other);

    PageLockMemory(PageLockMemory &&other);

    inline size_t size() {
        return nbytes;
    }

    void *data;
    size_t nbytes;
};
using PageLockMemoryPtr = std::shared_ptr<PageLockMemory>;
}

namespace faiss {

struct InvertedListsIterator {
    virtual ~InvertedListsIterator();
    virtual bool is_available() const = 0;
    virtual void next() = 0;
    virtual std::pair<idx_t, const uint8_t*> get_id_and_codes() = 0;
};

/** Table of inverted lists
 * multithreading rules:
 * - concurrent read accesses are allowed
 * - concurrent update accesses are allowed
 * - for resize and add_entries, only concurrent access to different lists
 *   are allowed
 */
struct InvertedLists {
    size_t nlist;     ///< number of possible key values
    size_t code_size; ///< code size per vector in bytes

    /// request to use iterator rather than get_codes / get_ids
    bool use_iterator = false;

    InvertedLists(size_t nlist, size_t code_size);

    virtual ~InvertedLists();

    /// used for BlockInvertedLists, where the codes are packed into groups
    /// and the individual code size is meaningless
    static const size_t INVALID_CODE_SIZE = static_cast<size_t>(-1);

    /*************************
     *  Read only functions */

    /// get the size of a list
    virtual size_t list_size(size_t list_no) const = 0;

    // get the segment number of a list (continuous storage can be regarded as 1-segment storage)
    virtual size_t get_segment_num(size_t list_no) const;

    // get the size of a segment in the given list (continuous storage can be regarded as 1-segment storage)
    virtual size_t get_segment_size(size_t list_no, size_t segment_no) const;

    // get the segment minimal number of a list (continuous storage can be regarded as 1-segment storage)
    virtual size_t get_segment_offset(size_t list_no, size_t segment_no) const;

    /** get the codes for an inverted list
     * must be released by release_codes
     *
     * @return codes    size list_size * code_size
     */
    virtual const uint8_t* get_codes(size_t list_no) const = 0;

    /** get the ids for an inverted list
     * must be released by release_ids
     *
     * @return ids      size list_size
     */
    virtual const idx_t* get_ids(size_t list_no) const = 0;

    /** get the codes slice beginning with offset for an inverted list
     *
     * @return codes    size : user guarantee the slice side by list_size or segment_size API
     */
    virtual const uint8_t* get_codes(size_t list_no, size_t offset) const;

    /**
     * get the code normal lengths for an inverted list
     * @param list_no
     * @param offset
     * @return
     */
    virtual const float* get_code_norms(size_t list_no, size_t offset) const;

    /** get the ids slice beginning with offset for an inverted list
     *
     * @return ids      size : user guarantee the slice side by list_size or segment_size API
     */
    virtual const idx_t* get_ids(size_t list_no, size_t offset) const;

    /// release codes returned by get_codes (default implementation is nop
    virtual void release_codes(size_t list_no, const uint8_t* codes) const;

    /// release code normals returned by get_code_norms (default implementation is nop
    virtual void release_code_norms(size_t list_no, const float* codes) const;

    /// release ids returned by get_ids
    virtual void release_ids(size_t list_no, const idx_t* ids) const;

    /// @return a single id in an inverted list
    virtual idx_t get_single_id(size_t list_no, size_t offset) const;

    /// @return a single code in an inverted list
    /// (should be deallocated with release_codes)
    virtual const uint8_t* get_single_code(size_t list_no, size_t offset) const;

    /// prepare the following lists (default does nothing)
    /// a list can be -1 hence the signed long
    virtual void prefetch_lists(const idx_t* list_nos, int nlist) const;

    /*****************************************
     * Iterator interface (with context)     */

    /// check if the list is empty
    virtual bool is_empty(size_t list_no, void* inverted_list_context = nullptr)
            const;

    /// get iterable for lists that use_iterator
    virtual InvertedListsIterator* get_iterator(
            size_t list_no,
            void* inverted_list_context = nullptr) const;

    /*************************
     * writing functions     */

    /// add one entry to an inverted list
    virtual size_t add_entry(
            size_t list_no, 
            idx_t theid, 
            const uint8_t* code,
            const float* code_norm = nullptr,
            void* inverted_list_context = nullptr);

    virtual size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code,
            const float* code_norm = nullptr) = 0;

    virtual void update_entry(
            size_t list_no,
            size_t offset,
            idx_t id,
            const uint8_t* code);

    virtual void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code) = 0;

    virtual void resize(size_t list_no, size_t new_size) = 0;

    virtual void reset();

    virtual InvertedLists* to_readonly();

    virtual bool is_readonly() const;

    /*************************
     * high level functions  */

    /// move all entries from oivf (empty on output)
    void merge_from(InvertedLists* oivf, size_t add_id);

    // how to copy a subset of elements from the inverted lists
    // This depends on two integers, a1 and a2.
    enum subset_type_t : int {
        // depends on IDs
        SUBSET_TYPE_ID_RANGE = 0, // copies ids in [a1, a2)
        SUBSET_TYPE_ID_MOD = 1,   // copies ids if id % a1 == a2
        // depends on order within invlists
        SUBSET_TYPE_ELEMENT_RANGE =
                2, // copies fractions of invlists so that a1 elements are left
                   // before and a2 after
        SUBSET_TYPE_INVLIST_FRACTION =
                3, // take fraction a2 out of a1 from each invlist, 0 <= a2 < a1
        // copy only inverted lists a1:a2
        SUBSET_TYPE_INVLIST = 4
    };

    /** copy a subset of the entries index to the other index
     * @return number of entries copied
     */
    size_t copy_subset_to(
            InvertedLists& other,
            subset_type_t subset_type,
            idx_t a1,
            idx_t a2) const;

    /*************************
     * statistics            */

    /// 1= perfectly balanced, >1: imbalanced
    double imbalance_factor() const;

    /// display some stats about the inverted lists
    void print_stats() const;

    /// sum up list sizes
    size_t compute_ntotal() const;

    /**************************************
     * Scoped inverted lists (for automatic deallocation)
     *
     * instead of writing:
     *
     *     uint8_t * codes = invlists->get_codes (10);
     *     ... use codes
     *     invlists->release_codes(10, codes)
     *
     * write:
     *
     *    ScopedCodes codes (invlists, 10);
     *    ... use codes.get()
     *    // release called automatically when codes goes out of scope
     *
     * the following function call also works:
     *
     *    foo (123, ScopedCodes (invlists, 10).get(), 456);
     *
     */

    struct ScopedIds {
        const InvertedLists* il;
        const idx_t* ids;
        size_t list_no;

        ScopedIds(const InvertedLists* il, size_t list_no)
                : il(il), ids(il->get_ids(list_no)), list_no(list_no) {}

        ScopedIds(const InvertedLists* il, size_t list_no, size_t offset)
                : il(il), ids(il->get_ids(list_no, offset)), list_no(list_no) {}

        const idx_t* get() {
            return ids;
        }

        idx_t operator[](size_t i) const {
            return ids[i];
        }

        ~ScopedIds() {
            il->release_ids(list_no, ids);
        }
    };

    struct ScopedCodes {
        const InvertedLists* il;
        const uint8_t* codes;
        size_t list_no;

        ScopedCodes(const InvertedLists* il, size_t list_no)
                : il(il), codes(il->get_codes(list_no)), list_no(list_no) {}

        ScopedCodes(const InvertedLists* il, size_t list_no, size_t offset)
                : il(il),
                  codes(il->get_single_code(list_no, offset)),
                  list_no(list_no) {}

        // For codes outside
        ScopedCodes(const InvertedLists *il, size_t list_no,
                    const uint8_t *original_codes)
                : il(il),
                  codes(original_codes),
                  list_no(list_no) {}

        const uint8_t* get() {
            return codes;
        }

        ~ScopedCodes() {
            il->release_codes(list_no, codes);
        }
    };

    struct ScopedCodeNorms {
        const InvertedLists* il;
        const float* code_norms;
        size_t list_no;

        ScopedCodeNorms(const InvertedLists* il, size_t list_no, size_t offset)
                : il(il),
                  code_norms(il->get_code_norms(list_no, offset)),
                  list_no(list_no) {}

        const float* get() {
            return code_norms;
        }

        ~ScopedCodeNorms() {
            il->release_code_norms(list_no, code_norms);
        }
    };
};

/// simple (default) implementation as an array of inverted lists
struct ArrayInvertedLists : InvertedLists {
    std::vector<std::vector<uint8_t>> codes; // binary codes, size nlist
    std::vector<std::vector<idx_t>> ids;     ///< Inverted lists for indexes

    bool with_norm = false;
    std::vector<std::vector<float>> code_norms; // code norms

    ArrayInvertedLists(size_t nlist, size_t code_size, bool with_norm = false);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    const float* get_code_norms(size_t list_no, size_t offset) const override;
    void release_code_norms(size_t list_no, const float* codes) const override;

    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code,
            const float* code_norm = nullptr) override;

    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code) override;

    void resize(size_t list_no, size_t new_size) override;

    InvertedLists* to_readonly() override;

    /// permute the inverted lists, map maps new_id to old_id
    void permute_invlists(const idx_t* map);

    bool is_empty(size_t list_no, void* inverted_list_context = nullptr)
            const override;

    ~ArrayInvertedLists() override;
};

// A Concurrent implementation for inverted lists
struct ConcurrentArrayInvertedLists : InvertedLists {
    template <typename T>
    struct Segment {
        Segment(size_t segment_size, size_t code_size) : segment_size_(segment_size), code_size_(code_size) {
            data_.reserve(segment_size_ * code_size_);
        }
        T& operator[](idx_t idx) {
            assert(idx < segment_size_);
            return data_[idx * code_size_];
        }
        const T& operator[](idx_t idx) const {
            assert(idx < segment_size_);
            return data_[idx * code_size_];
        }
        size_t segment_size_;
        size_t code_size_;
        std::vector<T> data_;
    };

    ConcurrentArrayInvertedLists(size_t nlist, size_t code_size, size_t segment_size, bool save_normal);

    size_t cal_segment_num(size_t capacity) const;
    void reserve(size_t list_no, size_t capacity);
    void shrink_to_fit(size_t list_no, size_t capacity);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    size_t get_segment_num(size_t list_no) const override;
    size_t get_segment_size(size_t list_no, size_t segment_no) const override;
    size_t get_segment_offset(size_t list_no, size_t segment_no) const override;

    const uint8_t* get_codes(size_t list_no, size_t offset) const override;
    const idx_t* get_ids(size_t list_no, size_t offset) const override;

    const float* get_code_norms(size_t list_no, size_t offset) const override;
    void release_code_norms(size_t list_no, const float* codes)
            const override;

    idx_t get_single_id(size_t list_no, size_t offset) const override;
    const uint8_t* get_single_code(size_t list_no, size_t offset) const override;

    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code,
            const float* code_norms = nullptr) override;

    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code) override;

    InvertedLists* to_readonly() override;

    void resize(size_t list_no, size_t new_size) override;

    ~ConcurrentArrayInvertedLists() override;

    size_t segment_size;
    bool save_norm;
    std::vector<std::atomic<size_t>> list_cur;
    std::vector<std::deque<Segment<uint8_t>>> codes;
    std::vector<std::deque<Segment<idx_t>>> ids;
    std::vector<std::deque<Segment<float>>> code_norms;
};

struct ReadOnlyArrayInvertedLists: InvertedLists {
    // for GPU
    PageLockMemoryPtr pin_readonly_codes;
    PageLockMemoryPtr pin_readonly_ids;

    // for CPU
    std::vector<uint8_t> readonly_codes;
    std::vector<idx_t> readonly_ids;

    std::vector<size_t> readonly_length;
    std::vector<size_t> readonly_offset;
    bool valid;

    ReadOnlyArrayInvertedLists(
            size_t nlist,
            size_t code_size,
            const std::vector<size_t>& list_length);

    explicit ReadOnlyArrayInvertedLists(const ArrayInvertedLists& other);
    explicit ReadOnlyArrayInvertedLists(const ArrayInvertedLists& other, bool offset);

    // Use default copy construct, just copy pointer, DON'T COPY pin_readonly_codes AND pin_readonly_ids
    // explicit ReadOnlyArrayInvertedLists(const ReadOnlyArrayInvertedLists &);
    // explicit ReadOnlyArrayInvertedLists(ReadOnlyArrayInvertedLists &&);
    virtual ~ReadOnlyArrayInvertedLists();

    size_t list_size(size_t list_no) const override;
    const uint8_t * get_codes(size_t list_no) const override;
    const idx_t * get_ids(size_t list_no) const override;

    const uint8_t * get_all_codes() const;
    const idx_t * get_all_ids() const;
    const std::vector<size_t>& get_list_length() const;

    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code,
            const float* code_norm = nullptr) override;

    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t *ids,
            const uint8_t *code) override;

    void resize(size_t list_no, size_t new_size) override;

    bool is_readonly() const override;

    bool is_valid();
};

/*****************************************************************
 * Meta-inverted lists
 *
 * About terminology: the inverted lists are seen as a sparse matrix,
 * that can be stacked horizontally, vertically and sliced.
 *****************************************************************/

/// invlists that fail for all write functions
struct ReadOnlyInvertedLists : InvertedLists {
    ReadOnlyInvertedLists(size_t nlist, size_t code_size)
            : InvertedLists(nlist, code_size) {}

    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code,
            const float* code_norm = nullptr) override;

    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code) override;

    void resize(size_t list_no, size_t new_size) override;
};

/// Horizontal stack of inverted lists
struct HStackInvertedLists : ReadOnlyInvertedLists {
    std::vector<const InvertedLists*> ils;

    /// build InvertedLists by concatenating nil of them
    HStackInvertedLists(int nil, const InvertedLists** ils);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    void prefetch_lists(const idx_t* list_nos, int nlist) const override;

    void release_codes(size_t list_no, const uint8_t* codes) const override;
    void release_ids(size_t list_no, const idx_t* ids) const override;

    idx_t get_single_id(size_t list_no, size_t offset) const override;

    const uint8_t* get_single_code(size_t list_no, size_t offset)
            const override;
};

using ConcatenatedInvertedLists = HStackInvertedLists;

/// vertical slice of indexes in another InvertedLists
struct SliceInvertedLists : ReadOnlyInvertedLists {
    const InvertedLists* il;
    idx_t i0, i1;

    SliceInvertedLists(const InvertedLists* il, idx_t i0, idx_t i1);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    void release_codes(size_t list_no, const uint8_t* codes) const override;
    void release_ids(size_t list_no, const idx_t* ids) const override;

    idx_t get_single_id(size_t list_no, size_t offset) const override;

    const uint8_t* get_single_code(size_t list_no, size_t offset)
            const override;

    void prefetch_lists(const idx_t* list_nos, int nlist) const override;
};

struct VStackInvertedLists : ReadOnlyInvertedLists {
    std::vector<const InvertedLists*> ils;
    std::vector<idx_t> cumsz;

    /// build InvertedLists by concatenating nil of them
    VStackInvertedLists(int nil, const InvertedLists** ils);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    void release_codes(size_t list_no, const uint8_t* codes) const override;
    void release_ids(size_t list_no, const idx_t* ids) const override;

    idx_t get_single_id(size_t list_no, size_t offset) const override;

    const uint8_t* get_single_code(size_t list_no, size_t offset)
            const override;

    void prefetch_lists(const idx_t* list_nos, int nlist) const override;
};

/** use the first inverted lists if they are non-empty otherwise use the second
 *
 * This is useful if il1 has a few inverted lists that are too long,
 * and that il0 has replacement lists for those, with empty lists for
 * the others. */
struct MaskedInvertedLists : ReadOnlyInvertedLists {
    const InvertedLists* il0;
    const InvertedLists* il1;

    MaskedInvertedLists(const InvertedLists* il0, const InvertedLists* il1);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    void release_codes(size_t list_no, const uint8_t* codes) const override;
    void release_ids(size_t list_no, const idx_t* ids) const override;

    idx_t get_single_id(size_t list_no, size_t offset) const override;

    const uint8_t* get_single_code(size_t list_no, size_t offset)
            const override;

    void prefetch_lists(const idx_t* list_nos, int nlist) const override;
};

/** if the inverted list in il is smaller than maxsize then return it,
 *  otherwise return an empty invlist */
struct StopWordsInvertedLists : ReadOnlyInvertedLists {
    const InvertedLists* il0;
    size_t maxsize;

    StopWordsInvertedLists(const InvertedLists* il, size_t maxsize);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    void release_codes(size_t list_no, const uint8_t* codes) const override;
    void release_ids(size_t list_no, const idx_t* ids) const override;

    idx_t get_single_id(size_t list_no, size_t offset) const override;

    const uint8_t* get_single_code(size_t list_no, size_t offset)
            const override;

    void prefetch_lists(const idx_t* list_nos, int nlist) const override;
};

} // namespace faiss

#endif
