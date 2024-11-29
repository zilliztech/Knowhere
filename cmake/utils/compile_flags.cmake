include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-std=gnu++17 COMPILER_SUPPORTS_CXX17)
set(CMAKE_CXX_FLAGS "-std=gnu++17 ${CMAKE_CXX_FLAGS}")

if(NOT COMPILER_SUPPORTS_CXX17)
  message(
    FATAL_ERROR
      "C++17 needed. Therefore a gcc compiler with a version higher than 4.3 is needed."
  )
endif()

if(WITH_ASAN)
  set(CMAKE_CXX_FLAGS
      "-fno-stack-protector -fno-omit-frame-pointer -fno-var-tracking -fsanitize=address ${CMAKE_CXX_FLAGS}"
  )
endif()

set(CMAKE_CXX_FLAGS "-Wall -fPIC ${CMAKE_CXX_FLAGS}")

if(__X86_64)
  set(CMAKE_CXX_FLAGS "-msse4.2 ${CMAKE_CXX_FLAGS}")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

if(WITH_CUVS)
  set(CMAKE_CUDA_FLAGS_DEBUG "-O0 -g -Xcompiler=-w ")
  set(CMAKE_CUDA_FLAGS_RELEASE "-O3 -DNDEBUG -Xcompiler=-w")
endif()
