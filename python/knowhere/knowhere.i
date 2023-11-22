
%module swigknowhere;

#pragma SWIG nowarn=321
#pragma SWIG nowarn=403
#pragma SWIG nowarn=325
#pragma SWIG nowarn=389
#pragma SWIG nowarn=341
#pragma SWIG nowarn=512
#pragma SWIG nowarn=362

%include <stdint.i>
typedef uint64_t size_t;
#define __restrict

%ignore knowhere::IndexFactory;
%ignore knowhere::IndexNode;
%ignore knowhere::Index;
%ignore knowhere::expected;
%{
#include <stdint.h>
#include <memory>
#ifdef SWIGPYTHON
#undef popcount64
#define SWIG_FILE_WITH_INIT
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>
#endif
#include <knowhere/expected.h>
#include <knowhere/factory.h>
#include <knowhere/version.h>
#include <knowhere/utils.h>
#include <knowhere/comp/brute_force.h>
#include <knowhere/comp/knowhere_config.h>
#include <knowhere/comp/local_file_manager.h>
#include <fstream>
#include <string>
using namespace knowhere;
%}

%{
#define SWIG_FILE_WITH_INIT
%}
%include "numpy.i"
%include "typemaps.i"
%init %{
import_array();
%}

%include <std_string.i>
%include <std_pair.i>
%include <std_map.i>
%include <std_shared_ptr.i>
%include <std_vector.i>
%include <exception.i>
%shared_ptr(knowhere::DataSet)
%shared_ptr(knowhere::BinarySet)
%template(DataSetPtr) std::shared_ptr<knowhere::DataSet>;
%template(BinarySetPtr) std::shared_ptr<knowhere::BinarySet>;
%template(int64_float_pair) std::pair<long long int, float>;
%include <knowhere/expected.h>
%include <knowhere/dataset.h>
%include <knowhere/binaryset.h>
%include <knowhere/bitsetview.h>
%include <knowhere/expected.h>

%apply (float* IN_ARRAY2, int DIM1, int DIM2) {(float* xb, int nb, int dim)}
%apply (int* IN_ARRAY2, int DIM1, int DIM2) {(int* xb, int nb, int dim)}
%apply (uint8_t *IN_ARRAY1, int DIM1) {(uint8_t *block, int size)}
%apply (int *IN_ARRAY1, int DIM1) {(int *lims, int len)}
%apply (int *IN_ARRAY1, int DIM1) {(int *ids, int len)}
%apply (float *IN_ARRAY1, int DIM1) {(float *dis, int len)}
%apply (float* INPLACE_ARRAY2, int DIM1, int DIM2){(float *dis,int nq_1,int k_1)}
%apply (int *INPLACE_ARRAY2, int DIM1, int DIM2){(int *ids,int nq_2,int k_2)}
%apply (float* INPLACE_ARRAY2, int DIM1, int DIM2){(float *data,int rows,int dim)}
%apply (int32_t *INPLACE_ARRAY2, int DIM1, int DIM2){(int32_t *data,int rows,int dim)}

%typemap(in, numinputs=0) knowhere::Status& status(knowhere::Status tmp) %{
    $1 = &tmp;
%}
%typemap(argout) knowhere::Status& status %{
    PyObject *o;
    o = PyInt_FromLong(long(*$1));
    $result = SWIG_Python_AppendOutput($result, o);
%}

%pythoncode %{
from enum import Enum
def redo(prefix):
    tmpD = { k : v for k,v in globals().items() if k.startswith(prefix + '_')}
    for k,v in tmpD.items():
        del globals()[k]
    tmpD = {k[len(prefix)+1:]:v for k,v in tmpD.items()}
    globals()[prefix] = Enum(prefix,tmpD)
redo('Status')
del redo
del Enum
%}

// use a empty json string as json default value
%typemap(default) std::string& json %{
   std::string default_json_str(knowhere::Json::object().dump());
   $1 = &default_json_str;
%}

%inline %{

class GILReleaser {
 public:
    GILReleaser() : save(PyEval_SaveThread()) {
    }
    ~GILReleaser() {
        PyEval_RestoreThread(save);
    }
    PyThreadState* save;
};

class AnnIteratorWrap {
 public:
    AnnIteratorWrap(std::shared_ptr<IndexNode::iterator> it = nullptr) : it_(it) {
        if (it_ == nullptr) {
            throw std::runtime_error("ann iterator must not be nullptr.");
        }
    }
    ~AnnIteratorWrap() {
    }

    bool HasNext() {
        return it_->HasNext();
    }

    std::pair<int64_t, float> Next() {
        return it_->Next();
    }

 private:
    std::shared_ptr<IndexNode::iterator> it_;
};

class IndexWrap {
 public:
    IndexWrap(const std::string& name, const int32_t& version) {
        GILReleaser rel;
        if (knowhere::UseDiskLoad(name, version)) {
            std::shared_ptr<knowhere::FileManager> file_manager = std::make_shared<knowhere::LocalFileManager>();
            auto diskann_pack = knowhere::Pack(file_manager);
            idx = IndexFactory::Instance().Create(name, version, diskann_pack);
        } else {
            idx = IndexFactory::Instance().Create(name, version);
        }
    }

    knowhere::Status
    Build(knowhere::DataSetPtr dataset, const std::string& json) {
        GILReleaser rel;
        return idx.Build(*dataset, knowhere::Json::parse(json));
    }

    knowhere::Status
    Train(knowhere::DataSetPtr dataset, const std::string& json) {
        GILReleaser rel;
        return idx.Train(*dataset, knowhere::Json::parse(json));
    }

    knowhere::Status
    Add(knowhere::DataSetPtr dataset, const std::string& json) {
        GILReleaser rel;
        return idx.Add(*dataset, knowhere::Json::parse(json));
    }

    knowhere::DataSetPtr
    Search(knowhere::DataSetPtr dataset, const std::string& json, const knowhere::BitsetView& bitset, knowhere::Status& status) {
        GILReleaser rel;
        auto res = idx.Search(*dataset, knowhere::Json::parse(json), bitset);
        if (res.has_value()) {
            status = knowhere::Status::success;
            return res.value();
        } else {
            status = res.error();
            return nullptr;
        }
    }

    std::vector<AnnIteratorWrap>
    GetAnnIterator(knowhere::DataSetPtr dataset, const std::string& json, const knowhere::BitsetView& bitset, knowhere::Status& status) {
        GILReleaser rel;
        auto res = idx.AnnIterator(*dataset, knowhere::Json::parse(json), bitset);
        std::vector<AnnIteratorWrap> result;
        if (!res.has_value()) {
            status = res.error();
            return result;
        }
        status = knowhere::Status::success;
        for (auto it : res.value()) {
            result.emplace_back(it);
        }
        return result;
    }

    knowhere::DataSetPtr
    RangeSearch(knowhere::DataSetPtr dataset, const std::string& json, const knowhere::BitsetView& bitset, knowhere::Status& status){
        GILReleaser rel;
        auto res = idx.RangeSearch(*dataset, knowhere::Json::parse(json), bitset);
        if (res.has_value()) {
            status = knowhere::Status::success;
            return res.value();
        } else {
            status = res.error();
            return nullptr;
        }
    }

    knowhere::DataSetPtr
    GetVectorByIds(knowhere::DataSetPtr dataset, knowhere::Status& status) {
        GILReleaser rel;
        auto res = idx.GetVectorByIds(*dataset);
        if (res.has_value()) {
            status = knowhere::Status::success;
            return res.value();
        } else {
            status = res.error();
            return nullptr;
        }
    }

    bool
    HasRawData(const std::string& metric_type) {
        GILReleaser rel;
        return idx.HasRawData(metric_type);
    }

    knowhere::Status
    Serialize(knowhere::BinarySetPtr binset) {
        GILReleaser rel;
        return idx.Serialize(*binset);
    }

    knowhere::Status
    Deserialize(knowhere::BinarySetPtr binset, const std::string& json) {
        GILReleaser rel;
        return idx.Deserialize(*binset, knowhere::Json::parse(json));
    }

    int64_t
    Dim() {
        return idx.Dim();
    }

    int64_t
    Size() {
        return idx.Size();
    }

    int64_t
    Count() {
        return idx.Count();
    }

    std::string
    Type() {
        return idx.Type();
    }

 private:
    Index<IndexNode> idx;
};

class BitSet {
 public:
    BitSet(const int num_bits) : num_bits_(num_bits) {
        bitset_.resize((num_bits_ + 7) / 8, 0);
    }

    void
    SetBit(const int idx) {
        bitset_[idx >> 3] |= 0x1 << (idx & 0x7);
    }

    knowhere::BitsetView
    GetBitSetView() {
        return knowhere::BitsetView(bitset_.data(), num_bits_);
    }

 private:
    std::vector<uint8_t> bitset_;
    int num_bits_ = 0;
};

knowhere::BitsetView
GetNullBitSetView() {
    return nullptr;
};

knowhere::DataSetPtr
Array2DataSetF(float* xb, int nb, int dim) {
    auto ds = std::make_shared<DataSet>();
    ds->SetIsOwner(false);
    ds->SetRows(nb);
    ds->SetDim(dim);
    ds->SetTensor(xb);
    return ds;
};

int32_t
CurrentVersion() {
    return knowhere::Version::GetCurrentVersion().VersionNumber();
}

knowhere::DataSetPtr
Array2DataSetI(int *xb, int nb, int dim){
    auto ds = std::make_shared<DataSet>();
    ds->SetIsOwner(false);
    ds->SetRows(nb);
    ds->SetDim(dim*32);
    ds->SetTensor(xb);
    return ds;
};

knowhere::DataSetPtr
Array2DataSetIds(int* ids, int len){
    auto ds = std::make_shared<DataSet>();
    ds->SetIsOwner(true);
    ds->SetRows(len);

    int64_t* ids_ = new int64_t[len];
    for (int i = 0; i < len; i++) {
        ids_[i] = (int64_t)ids[i];
    }
    ds->SetIds(ids_);
    return ds;
};

int64_t
DataSet_Rows(knowhere::DataSetPtr results){
    return results->GetRows();
}

int64_t
DataSet_Dim(knowhere::DataSetPtr results){
    return results->GetDim();
}

knowhere::BinarySetPtr
GetBinarySet() {
    return std::make_shared<knowhere::BinarySet>();
}

knowhere::DataSetPtr
GetNullDataSet() {
    return nullptr;
}

void
DataSet2Array(knowhere::DataSetPtr result, float* dis, int nq_1, int k_1, int* ids, int nq_2, int k_2) {
    GILReleaser rel;
    auto ids_ = result->GetIds();
    auto dist_ = result->GetDistance();
    assert(nq_1 == nq_2);
    assert(k_1 == k_2);
    for (int i = 0; i < nq_1; i++) {
        for (int j = 0; j < k_1; ++j) {
            *(ids + i * k_1 + j) = *((int64_t*)(ids_) + i * k_1 + j);
            *(dis + i * k_1 + j) = *((float*)(dist_) + i * k_1 + j);
        }
    }
}

void
DataSetTensor2Array(knowhere::DataSetPtr result, float* data, int rows, int dim) {
    GILReleaser rel;
    auto data_ = result->GetTensor();
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < dim; ++j) {
            *(data + i * dim + j) = *((float*)(data_) + i * dim + j);
        }
    }
}

void
BinaryDataSetTensor2Array(knowhere::DataSetPtr result, int32_t* data, int rows, int dim) {
    GILReleaser rel;
    auto data_ = result->GetTensor();
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < dim; ++j) {
            *(data + i * dim + j) = *((int32_t*)(data_) + i * dim + j);
        }
    }
}

void
DumpRangeResultIds(knowhere::DataSetPtr result, int* ids, int len) {
    GILReleaser rel;
    auto ids_ = result->GetIds();
    for (int i = 0; i < len; ++i) {
        *(ids + i) = *((int64_t*)(ids_) + i);
    }
}

void
DumpRangeResultLimits(knowhere::DataSetPtr result, int* lims, int len) {
    GILReleaser rel;
    auto lims_ = result->GetLims();
    for (int i = 0; i < len; ++i) {
        *(lims + i) = *((size_t*)(lims_) + i);
    }
}

void
DumpRangeResultDis(knowhere::DataSetPtr result, float* dis, int len) {
    GILReleaser rel;
    auto dist_ = result->GetDistance();
    for (int i = 0; i < len; ++i) {
        *(dis + i) = *((float*)(dist_) + i);
    }
}

void
Dump(knowhere::BinarySetPtr binset, const std::string& file_name) {
    auto binary_set = *binset;
    auto binary_map = binset -> binary_map_;
    std::ofstream outfile;
    outfile.open(file_name, std::ios::out | std::ios::trunc);
    if (outfile.good()) {
        for (auto it = binary_map.begin(); it != binary_map.end(); ++it) {
            // serialization: name_length(size_t); name(char[]); binset_size(size_t); binset(uint8[]);
            auto name = it->first;
            uint64_t name_len = name.size();
            outfile << name_len;
            outfile << name;
            auto value = it->second;
            outfile << value->size;
            outfile.write(reinterpret_cast<char*>(value->data.get()), value->size);
        }
        // end with 0
        outfile << 0;
        outfile.flush();
    }
}

void
Load(knowhere::BinarySetPtr binset, const std::string& file_name) {
    std::ifstream infile;
    infile.open(file_name, std::ios::in);
    if (infile.good()) {
        uint64_t name_len;
        while (true) {
            // deserialization: name_length(size_t); name(char[]); binset_size(size_t); binset(uint8[]);
            infile >> name_len;
            if (name_len == 0) break;

            auto _name = new char[name_len];
            infile.read(_name, name_len);
            std::string name(_name, name_len);

            int64_t size;
            infile >> size;
            if (size > 0) {
                auto data = new uint8_t[size];
                std::shared_ptr<uint8_t[]> data_ptr(data);
                infile.read(reinterpret_cast<char*>(data_ptr.get()), size);
                binset->Append(name, data_ptr, size);
            }
        }
    }
}

knowhere::DataSetPtr
BruteForceSearch(knowhere::DataSetPtr base_dataset, knowhere::DataSetPtr query_dataset, const std::string& json,
                 const knowhere::BitsetView& bitset, knowhere::Status& status) {
    GILReleaser rel;
    auto res = knowhere::BruteForce::Search(base_dataset, query_dataset, knowhere::Json::parse(json), bitset);
    if (res.has_value()) {
        status = knowhere::Status::success;
        return res.value();
    } else {
        status = res.error();
        return nullptr;
    }
}

knowhere::DataSetPtr
BruteForceRangeSearch(knowhere::DataSetPtr base_dataset, knowhere::DataSetPtr query_dataset, const std::string& json,
                      const knowhere::BitsetView& bitset, knowhere::Status& status) {
    GILReleaser rel;
    auto res = knowhere::BruteForce::RangeSearch(base_dataset, query_dataset, knowhere::Json::parse(json), bitset);
    if (res.has_value()) {
        status = knowhere::Status::success;
        return res.value();
    } else {
        status = res.error();
        return nullptr;
    }
}

void
SetSimdType(const std::string type) {
    if (type == "auto") {
        knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::AUTO);
    } else if (type == "avx512") {
        knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::AVX512);
    } else if (type == "avx2") {
        knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::AVX2);
    } else if (type == "avx" || type == "sse4_2") {
        knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::SSE4_2);
    } else {
        knowhere::KnowhereConfig::SetSimdType(knowhere::KnowhereConfig::SimdType::GENERIC);
    }
}

%}

%template(AnnIteratorWrapVector) std::vector<AnnIteratorWrap>;
