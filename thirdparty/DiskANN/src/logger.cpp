// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <cstring>
#include <iostream>

#include "diskann/logger_impl.h"

namespace diskann {

  ANNStreamBuf coutBuff(stdout);
  ANNStreamBuf cerrBuff(stderr);

  std::basic_ostream<char> cout(&coutBuff);
  std::basic_ostream<char> cerr(&cerrBuff);

  ANNStreamBuf::ANNStreamBuf(FILE* fp) {
    if (fp == nullptr) {
      throw diskann::ANNException(
          "File pointer passed to ANNStreamBuf() cannot be null", -1);
    }
    if (fp != stdout && fp != stderr) {
      throw diskann::ANNException(
          "The custom logger only supports stdout and stderr.", -1);
    }
    _fp = fp;
    _logLevel = (_fp == stdout) ? ANNIndex::LogLevel::LL_Info
                                : ANNIndex::LogLevel::LL_Error;
    _buf = std::make_unique<char[]>(BUFFER_SIZE);

    std::memset(_buf.get(), 0, (BUFFER_SIZE) * sizeof(char));
    setp(_buf.get(), _buf.get() + BUFFER_SIZE);
  }

  ANNStreamBuf::~ANNStreamBuf() {
    sync();
    _fp = nullptr;  // we'll not close because we can't.
  }

  int ANNStreamBuf::overflow(int c) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (c != EOF) {
      *pptr() = (char) c;
      pbump(1);
    }
    flush();
    return c;
  }

  int ANNStreamBuf::sync() {
    std::lock_guard<std::mutex> lock(_mutex);
    flush();
    return 0;
  }

  int ANNStreamBuf::underflow() {
    throw diskann::ANNException(
        "Attempt to read on streambuf meant only for writing.", -1);
  }

  int ANNStreamBuf::flush() {
    const int num = (int) (pptr() - pbase());
    logImpl(pbase(), num);
    pbump(-num);
    return num;
  }
  void ANNStreamBuf::logImpl(char* str, int num) {
    fwrite(str, sizeof(char), num, _fp);
    fflush(_fp);
  }

}  // namespace diskann
