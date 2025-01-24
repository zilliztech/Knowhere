#pragma once

#include <faiss/impl/io.h>
#include <cstdint>

namespace faiss {

struct ZeroCopyIOReader : public faiss::IOReader {
    uint8_t* data_;
    size_t rp_ = 0;
    size_t total_ = 0;

    ZeroCopyIOReader(uint8_t* data, size_t size);
    ~ZeroCopyIOReader();

    void reset();
    size_t getDataView(void** ptr, size_t size, size_t nitems);
    size_t operator()(void* ptr, size_t size, size_t nitems) override;

    int filedescriptor() override;
};

}  // namespace faiss