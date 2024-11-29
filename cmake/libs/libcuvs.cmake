# =============================================================================
# Copyright (c) 2023, NVIDIA CORPORATION.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

add_definitions(-DKNOWHERE_WITH_CUVS)
set(CUVS_VERSION "${RAPIDS_VERSION}")
set(CUVS_FORK "rapidsai")
set(CUVS_PINNED_TAG "branch-24.12")

rapids_find_package(CUDAToolkit REQUIRED BUILD_EXPORT_SET knowhere-exports
                    INSTALL_EXPORT_SET knowhere-exports)

function(find_and_configure_cuvs)
  set(oneValueArgs VERSION FORK PINNED_TAG BUILD_MG_ALGOS)
  cmake_parse_arguments(PKG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  # -----------------------------------------------------
  # Invoke CPM find_package()
  # -----------------------------------------------------
  rapids_cpm_find(
    cuvs
    ${PKG_VERSION}
    GLOBAL_TARGETS
    cuvs::cuvs
    BUILD_EXPORT_SET    knowhere-exports
    INSTALL_EXPORT_SET  knowhere-exports
    COMPONENTS
    cuvs_static
    CPM_ARGS
    GIT_REPOSITORY
    https://github.com/${PKG_FORK}/cuvs.git
    GIT_TAG
    ${PKG_PINNED_TAG}
    SOURCE_SUBDIR
    cpp
    OPTIONS
    "BUILD_C_LIBRARY OFF"
    "BUILD_TESTS OFF"
    "BUILD_BENCH OFF"
    "BUILD_MG_ALGOS ${PKG_BUILD_MG_ALGOS}"
    "CUVS_USE_FAISS_STATIC OFF" # Turn this on to build FAISS into your binary
    "CUVS_NVTX OFF")

  if(cuvs_ADDED)
    message(VERBOSE "KNOWHERE: Using CUVS located in ${cuvs_SOURCE_DIR}")
  else()
    message(VERBOSE "KNOWHERE: Using CUVS located in ${cuvs_DIR}")
  endif()
endfunction()

# Change pinned tag here to test a commit in CI To use a different CUVS locally,
# set the CMake variable CPM_cuvs_SOURCE=/path/to/local/cuvs
find_and_configure_cuvs(
  VERSION
  ${CUVS_VERSION}.00
  FORK
  ${CUVS_FORK}
  PINNED_TAG
  ${CUVS_PINNED_TAG}
  BUILD_MG_ALGOS
  OFF) # TODO: Implement MG algos
