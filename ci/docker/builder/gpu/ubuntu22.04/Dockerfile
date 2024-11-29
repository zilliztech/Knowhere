FROM nvidia/cuda:12.2.2-devel-ubuntu22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV CMAKE_VERSION="v3.28.5"
ENV CMAKE_TAR="cmake-3.28.5-linux-x86_64.tar.gz"
ENV CCACHE_VERSION="v4.9.1"
ENV CCACHE_DIR="ccache-4.9.1-linux-x86_64"
ENV CCACHE_TAR="ccache-4.9.1-linux-x86_64.tar.xz"
ENV BFLOAT16_WHL="bfloat16-1.4.0-cp311-cp311-linux_x86_64.whl"

RUN apt update \
    && apt install -y ca-certificates apt-transport-https software-properties-common lsb-release \
    && apt install -y --no-install-recommends wget curl git make gfortran gcc g++ swig \
    && apt install -y gcc-12 g++-12 \
    && apt install -y python3.11 python3.11-dev python3.11-distutils \
    && apt install -y python3-setuptools \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 110 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 120 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 110 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 120 \
    && update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.10 310 \
    && update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.11 311 \
    && curl -sS https://bootstrap.pypa.io/get-pip.py | python3 \
    && export PATH=$PATH:$HOME/.local/bin \
    && pip3 install wheel \
    && apt remove --purge -y \
    && rm -rf /var/lib/apt/lists/*

# install cmake, ccache and bfloat16
RUN cd /tmp \
    && wget https://github.com/Kitware/CMake/releases/download/${CMAKE_VERSION}/${CMAKE_TAR} \
    && tar --strip-components=1 -xz -C /usr/local -f ${CMAKE_TAR} \
    && rm -f ${CMAKE_TAR} \
    && wget https://github.com/ccache/ccache/releases/download/${CCACHE_VERSION}/${CCACHE_TAR} \
    && tar -xf ${CCACHE_TAR} \
    && cp ${CCACHE_DIR}/ccache /usr/local/bin \
    && rm -f ${CCACHE_TAR} \
    && wget https://github.com/zilliztech/knowhere/releases/download/v2.3.1/${BFLOAT16_WHL} \
    && pip3 install ${BFLOAT16_WHL} \
    && rm -f ${BFLOAT16_WHL}

# install knowhere dependancies
RUN apt update \
    && apt install -y libopenblas-openmp-dev libcurl4-openssl-dev libaio-dev libevent-dev lcov \
    && pip3 install conan==1.61.0 \
    && conan remote add default-conan-local https://milvus01.jfrog.io/artifactory/api/conan/default-conan-local

# clone knowhere repo and build to update .conan
RUN git clone https://github.com/zilliztech/knowhere.git \
    && cd knowhere \
    && mkdir build && cd build \
    && conan install .. --build=missing -o with_ut=True -o with_diskann=True -o with_cuvs=True -s compiler.libcxx=libstdc++11 -s build_type=Release \
    && conan build .. \
    && cd ../.. && rm -rf knowhere
