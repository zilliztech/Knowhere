knowhere_file_glob(
  GLOB FAISS_SRCS thirdparty/faiss/faiss/*.cpp
  thirdparty/faiss/faiss/impl/*.cpp thirdparty/faiss/faiss/invlists/*.cpp
  thirdparty/faiss/faiss/utils/*.cpp
  thirdparty/faiss/faiss/cppcontrib/knowhere/*.cpp)

knowhere_file_glob(GLOB FAISS_AVX512_SRCS
                   thirdparty/faiss/faiss/impl/*avx512.cpp)

knowhere_file_glob(
  GLOB
  FAISS_AVX2_SRCS
  thirdparty/faiss/faiss/impl/*avx.cpp
  thirdparty/faiss/faiss/impl/pq4_fast_scan_search_1.cpp
  thirdparty/faiss/faiss/impl/pq4_fast_scan_search_qbs.cpp
  thirdparty/faiss/faiss/utils/partitioning_avx2.cpp
  thirdparty/faiss/faiss/IndexPQFastScan.cpp
  thirdparty/faiss/faiss/IndexIVFFastScan.cpp
  thirdparty/faiss/faiss/IndexIVFPQFastScan.cpp)

list(REMOVE_ITEM FAISS_SRCS ${FAISS_AVX512_SRCS})

# disable RHNSW
knowhere_file_glob(GLOB FAISS_RHNSW_SRCS thirdparty/faiss/faiss/impl/RHNSW.cpp)
list(REMOVE_ITEM FAISS_SRCS ${FAISS_RHNSW_SRCS})

if(__X86_64)
  set(UTILS_SRC src/simd/distances_ref.cc src/simd/hook.cc)
  set(UTILS_SSE_SRC src/simd/distances_sse.cc)
  set(UTILS_AVX_SRC src/simd/distances_avx.cc)
  set(UTILS_AVX512_SRC src/simd/distances_avx512.cc)

  add_library(utils_sse OBJECT ${UTILS_SSE_SRC})
  add_library(utils_avx OBJECT ${UTILS_AVX_SRC})
  add_library(utils_avx512 OBJECT ${UTILS_AVX512_SRC})

  target_compile_options(utils_sse PRIVATE -msse4.2 -mpopcnt)
  target_compile_options(utils_avx PRIVATE -mfma -mf16c -mavx2 -mpopcnt)
  target_compile_options(utils_avx512 PRIVATE -mfma -mf16c -mavx512f -mavx512dq
                                              -mavx512bw -mpopcnt -mavx512vl)

  add_library(
    knowhere_utils STATIC
    ${UTILS_SRC} $<TARGET_OBJECTS:utils_sse> $<TARGET_OBJECTS:utils_avx>
    $<TARGET_OBJECTS:utils_avx512>)
  target_link_libraries(knowhere_utils PUBLIC glog::glog)
endif()

if(__AARCH64)

  set(UTILS_SRC src/simd/distances_ref.cc src/simd/distances_neon.cc)
  set(UTILS_SVE_SRC src/simd/hook.cc src/simd/distances_sve.cc)
  set(ALL_UTILS_SRC ${UTILS_SRC} ${UTILS_SVE_SRC})

  add_library(
    knowhere_utils STATIC
    ${ALL_UTILS_SRC}
  )

  foreach(SVE_FILE ${UTILS_SVE_SRC})
    set_source_files_properties(${SVE_FILE} PROPERTIES COMPILE_OPTIONS "-march=armv8-a+sve")
  endforeach()

  target_compile_options(knowhere_utils PRIVATE -march=armv8-a)
  target_link_libraries(knowhere_utils PUBLIC glog::glog)
endif()


# ToDo: Add distances_vsx.cc for powerpc64 SIMD acceleration
if(__PPC64)
  set(UTILS_SRC src/simd/hook.cc src/simd/distances_ref.cc src/simd/distances_powerpc.cc)
  add_library(knowhere_utils STATIC ${UTILS_SRC})
  target_link_libraries(knowhere_utils PUBLIC glog::glog)
endif()


if(LINUX)
  set(BLA_VENDOR OpenBLAS)
endif()

if(APPLE)
  set(BLA_VENDOR Apple)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Android" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  find_package(OpenBLAS REQUIRED)
  set(BLAS_LIBRARIES OpenBLAS::OpenBLAS)
else()
  find_package(LAPACK REQUIRED)
  find_package(BLAS REQUIRED)
endif()

if(__X86_64)
  list(REMOVE_ITEM FAISS_SRCS ${FAISS_AVX2_SRCS})

  knowhere_file_glob(GLOB FAISS_NEON_SRCS thirdparty/faiss/faiss/impl/*neon.cpp)
  list(REMOVE_ITEM FAISS_SRCS ${FAISS_NEON_SRCS})

  add_library(faiss_avx2 OBJECT ${FAISS_AVX2_SRCS})
  target_compile_options(faiss_avx2 PRIVATE $<$<COMPILE_LANGUAGE:CXX>: -msse4.2
                                            -mavx2 -mfma -mf16c -mpopcnt>)
  add_library(faiss_avx512 OBJECT ${FAISS_AVX512_SRCS})
  target_compile_options(
    faiss_avx512
    PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -msse4.2
            -mavx2
            -mfma
            -mf16c
            -mavx512f
            -mavx512dq
            -mavx512bw
            -mavx512vl
            -mpopcnt>)

  add_library(faiss STATIC ${FAISS_SRCS})

  add_dependencies(faiss faiss_avx2 faiss_avx512 knowhere_utils)
  target_compile_options(
    faiss
    PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -msse4.2
            -mpopcnt
            -Wno-sign-compare
            -Wno-unused-variable
            -Wno-reorder
            -Wno-unused-local-typedefs
            -Wno-unused-function
            -Wno-strict-aliasing>)
  target_link_libraries(
    faiss PUBLIC OpenMP::OpenMP_CXX ${BLAS_LIBRARIES} ${LAPACK_LIBRARIES}
                 faiss_avx2 faiss_avx512 knowhere_utils)
  target_compile_definitions(faiss PRIVATE FINTEGER=int)
endif()

if(__AARCH64)
  knowhere_file_glob(GLOB FAISS_AVX_SRCS thirdparty/faiss/faiss/impl/*avx.cpp)
  list(REMOVE_ITEM FAISS_SRCS ${FAISS_AVX_SRCS})

  add_library(faiss STATIC ${FAISS_SRCS})

  target_compile_options(
    faiss
    PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -Wno-sign-compare
            -Wno-unused-variable
            -Wno-reorder
            -Wno-unused-local-typedefs
            -Wno-unused-function
            -Wno-strict-aliasing>)

  add_dependencies(faiss knowhere_utils)
  target_link_libraries(faiss PUBLIC OpenMP::OpenMP_CXX ${BLAS_LIBRARIES} ${LAPACK_LIBRARIES}
                                     knowhere_utils)
  target_compile_definitions(faiss PRIVATE FINTEGER=int)
endif()

if(__PPC64)
  knowhere_file_glob(GLOB FAISS_AVX_SRCS thirdparty/faiss/faiss/impl/*avx.cpp)
  list(REMOVE_ITEM FAISS_SRCS ${FAISS_AVX_SRCS})

  knowhere_file_glob(GLOB FAISS_NEON_SRCS thirdparty/faiss/faiss/impl/*neon.cpp)
  list(REMOVE_ITEM FAISS_SRCS ${FAISS_NEON_SRCS})

  add_library(faiss STATIC ${FAISS_SRCS})

  target_compile_options(
    faiss
    PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -mcpu=native
            -mvsx
            -Wno-sign-compare
            -Wno-unused-variable
            -Wno-reorder
            -Wno-unused-local-typedefs
            -Wno-unused-function
            -Wno-strict-aliasing>)

  add_dependencies(faiss knowhere_utils)
  target_link_libraries(faiss PUBLIC OpenMP::OpenMP_CXX ${BLAS_LIBRARIES} ${LAPACK_LIBRARIES}
                                      knowhere_utils)
  target_compile_definitions(faiss PRIVATE FINTEGER=int)
endif()
