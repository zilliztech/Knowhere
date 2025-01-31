FROM nvidia/cuda:11.8.0-devel-ubuntu20.04

ENV DEBIAN_FRONTEND=noninteractive
ENV CMAKE_VERSION="v3.27"
ENV CMAKE_TAR="cmake-3.27.5-linux-x86_64.tar.gz"
ENV CCACHE_VERSION="v4.9.1"
ENV CCACHE_TAR="ccache-4.9.1-linux-x86_64.tar.xz"
RUN apt update \
    && apt install -y ca-certificates apt-transport-https software-properties-common lsb-release \
    && gpg --list-keys \
    && gpg --no-default-keyring --keyring /usr/share/keyrings/deadsnakes.gpg \
           --keyserver keyserver.ubuntu.com --recv-keys F23C5A6CF475977595C89F51BA6932366A755776 \
    && echo "deb [signed-by=/usr/share/keyrings/deadsnakes.gpg] https://ppa.launchpadcontent.net/deadsnakes/ppa/ubuntu \
       $(lsb_release -cs) main" | tee /etc/apt/sources.list.d/python.list \
    && apt update \
    && apt install -y python3.11 python3.11-dev python3.11-distutils \
    && apt install -y python3-setuptools \
    && cd /usr/bin \
    && unlink python3 && ln -s python3.11 python3 \
    && apt install -y --no-install-recommends wget curl git g++ gcc make gfortran swig \
    && apt remove --purge -y \
    && rm -rf /var/lib/apt/lists/*
RUN cd /tmp && wget --tries=3 --retry-connrefused "https://cmake.org/files/${CMAKE_VERSION}/${CMAKE_TAR}" \
    && tar --strip-components=1 -xz -C /usr/local -f ${CMAKE_TAR} \
    && rm -f ${CMAKE_TAR}
RUN cd /tmp && wget --tries=3 --retry-connrefused "https://github.com/ccache/ccache/releases/download/${CCACHE_VERSION}/${CCACHE_TAR}" \
    && tar -xf ${CCACHE_TAR} \
    && cp ccache-4.9.1-linux-x86_64/ccache /usr/local/bin \
    && rm -f ${CCACHE_TAR}
RUN curl -sS https://bootstrap.pypa.io/get-pip.py | python3 \
    && pip install wheel

# install knowhere dependancies
RUN apt update \
    && apt install -y libopenblas-openmp-dev libcurl4-openssl-dev libaio-dev libevent-dev lcov \
    && pip3 install conan==1.61.0 \
    && conan remote add default-conan-local https://milvus01.jfrog.io/artifactory/api/conan/default-conan-local \
    && export PATH=$PATH:$HOME/.local/bin

# clone knowhere repo and build to update .conan
RUN git clone https://github.com/zilliztech/knowhere.git \
    && cd knowhere \
    && mkdir build && cd build \
    && conan install .. --build=missing -o with_ut=True -o with_diskann=True -o with_cuvs=True -s compiler.libcxx=libstdc++11 -s build_type=Release \
    && conan build .. \
    && cd ../.. \
    && rm -rf knowhere
