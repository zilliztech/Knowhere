int total_timeout_minutes = 240
def knowhere_wheel=''
pipeline {
    agent {
        kubernetes {
            cloud "new_ci_idc"
            inheritFrom 'default'
            yamlFile 'ci/pod/ut-gpu.yaml'
            defaultContainer 'main'
        }
    }

    options {
        timeout(time: total_timeout_minutes, unit: 'MINUTES')
        buildDiscarder logRotator(artifactDaysToKeepStr: '30')
        parallelsAlwaysFailFast()
        disableConcurrentBuilds(abortPrevious: true)
        preserveStashes(buildCount: 10)
    }
    stages {
        stage("UT"){
            steps {
                container("build"){
                    script{
                        def date = sh(returnStdout: true, script: 'date +%Y%m%d').trim()
                        def gitShortCommit = sh(returnStdout: true, script: "echo ${env.GIT_COMMIT} | cut -b 1-7 ").trim()
                        version="${env.CHANGE_ID}.${date}.${gitShortCommit}"
                        sh "apt-get update || true"
                        sh "apt-get install libaio-dev libcurl4-openssl-dev libdouble-conversion-dev libevent-dev libgflags-dev git -y"
                        sh "pip3 install conan==1.61.0"
                        sh "conan remote add default-conan-local https://milvus01.jfrog.io/artifactory/api/conan/default-conan-local"
                        sh "cmake --version"
                        sh "nvidia-smi --query-gpu=name --format=csv,noheader"
                        sh "./scripts/prepare_gpu_build.sh"
                        sh "mkdir build"
                        sh "cd build/ && conan install .. --build=missing -s build_type=Release -o with_diskann=True -o with_ut=True -o with_cuvs=True -s compiler.libcxx=libstdc++11 \
                              && conan build .. \
                              && ./Release/tests/ut/knowhere_tests"
                    }
                }
            }
        }
    }
}
