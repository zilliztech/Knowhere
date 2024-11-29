<p>
    <img src="static/knowhere-logo.png" alt="Knowhere Logo"/>
</p>

This document will help you to build the Knowhere repository from source code and to run unit tests. Please [file an issue](https://github.com/zilliztech/knowhere/issues/new) if there's a problem.

## Introduction

Knowhere is written in C++. It is an independent project that act as Milvus's internal core.

## Building Knowhere Within Milvus

If you wish to only use Knowhere within Milvus without changing any of the Knowhere source code, we suggest that you move to the [Milvus main project](https://github.com/milvus-io/milvus) and build Milvus directly, where Knowhere is then built implicitly during Milvus build.

## System Requirements

All Linux distributions are available for Knowhere development. However, a majority of our contributor worked with Ubuntu or CentOS systems, with a small portion of Mac (both x86_64 and Apple Silicon) contributors. If you would like Knowhere to build and run on other distributions, you are more than welcome to file an issue and contribute!

Here's a list of verified OS types where Knowhere can successfully build and run:

- Ubuntu 20.04 x86_64
- Ubuntu 20.04 Aarch64
- MacOS (x86_64)
- MacOS (Apple Silicon)

## Building Knowhere From Source Code

#### Install Dependencies

```bash
$ sudo apt install build-essential libopenblas-openmp-dev libaio-dev python3-dev python3-pip
$ pip3 install conan==1.61.0 --user
$ export PATH=$PATH:$HOME/.local/bin
```

#### Build From Source Code

* Ubuntu 20.04

```bash
$ mkdir build && cd build
#add conan remote
$ conan remote add default-conan-local https://milvus01.jfrog.io/artifactory/api/conan/default-conan-local
#DEBUG CPU
$ conan install .. --build=missing -o with_ut=True -s compiler.libcxx=libstdc++11 -s build_type=Debug
#RELEASE CPU
$ conan install .. --build=missing -o with_ut=True -s compiler.libcxx=libstdc++11 -s build_type=Release
#DEBUG GPU
$ conan install .. --build=missing -o with_ut=True -o with_cuvs=True -s compiler.libcxx=libstdc++11 -s build_type=Debug
#RELEASE GPU
$ conan install .. --build=missing -o with_ut=True -o with_cuvs=True -s compiler.libcxx=libstdc++11 -s build_type=Release
#DISKANN SUPPORT
$ conan install .. --build=missing -o with_ut=True -o with_diskann=True -s compiler.libcxx=libstdc++11 -s build_type=Debug/Release
#build with conan
$ conan build ..
#verbose
export VERBOSE=1
```

* MacOS

```bash
#RELEASE CPU
conan install .. --build=missing -o with_ut=True -s compiler.libcxx=libc++ -s build_type=Release
#DEBUG CPU
conan install .. --build=missing -o with_ut=True -s compiler.libcxx=libc++ -s build_type=Debug
#build with conan
conan build ..
```

#### Running Unit Tests

```bash
# in build directories
#Debug
$ ./Debug/tests/ut/knowhere_tests
#Release
$ ./Release/tests/ut/knowhere_tests
```

#### Clean up

```bash
$ git clean -fxd
```

## GEN PYTHON WHEEL(NEED REALSE BUILD)

install dependency:

```
sudo apt install swig python3-dev
pip3 install bfloat16
```

after build knowhere:

```bash
cd python
python3 setup.py bdist_wheel
```

install knowhere wheel:

```bash
pip3 install dist/pyknowhere-0.0.0-cp38-cp38-linux_x86_64.whl
```

clean

```bash
cd python
rm -rf build
rm -rf dist
rm -rf knowhere.egg-info
rm knowhere/knowhere_wrap.cpp
rm knowhere/swigknowhere.py
```

## Contributing

### Pre-Commit

Before submitting a pull request, please make sure running pre-commit checks locally to ensure the code is ready for review. Use the following command to install pre-commit checks:

```bash
pip3 install pre-commit
pre-commit install --hook-type pre-commit --hook-type pre-push

# If clang-format and clang-tidy not already installed:
# linux
apt install clang-format clang-tidy
# mac
brew install llvm
ln -s "$(brew --prefix llvm)/bin/clang-format" "/usr/local/bin/clang-format"
ln -s "$(brew --prefix llvm)/bin/clang-tidy" "/usr/local/bin/clang-tidy"
```
