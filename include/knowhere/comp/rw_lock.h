//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.
#ifndef KNOWHERE_RW_LOCK_H
#define KNOWHERE_RW_LOCK_H
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
/*
FairRWLock is a fair MultiRead-SingleWrite lock
*/
namespace knowhere {
class FairRWLock {
 public:
    void
    LockRead() {
        std::unique_lock lock(mtx_);
        auto cur_id = task_id_counter_++;
        task_id_q.push(cur_id);
        task_cv_.wait(lock, [&] { return task_id_q.front() == cur_id && !have_writer_task_; });
        reader_task_counter_++;
        task_id_q.pop();
    }
    void
    UnLockRead() {
        std::unique_lock lock(mtx_);
        if (--reader_task_counter_ == 0) {
            task_cv_.notify_all();
        }
    }
    void
    LockWrite() {
        std::unique_lock lock(mtx_);
        auto cur_id = task_id_counter_++;
        task_id_q.push(cur_id);
        task_cv_.wait(lock,
                      [&] { return task_id_q.front() == cur_id && !have_writer_task_ && reader_task_counter_ == 0; });
        have_writer_task_ = true;
        task_id_q.pop();
    }
    void
    UnLockWrite() {
        std::unique_lock lock(mtx_);
        have_writer_task_ = false;
        task_cv_.notify_all();
    }

 private:
    uint64_t task_id_counter_ = 0;
    uint64_t reader_task_counter_ = 0;
    bool have_writer_task_ = false;
    std::mutex mtx_;
    std::condition_variable task_cv_;
    std::queue<uint64_t> task_id_q;
};

class FairReadLockGuard {
 public:
    explicit FairReadLockGuard(FairRWLock& lock) : lock_(lock) {
        lock_.LockRead();
    }

    ~FairReadLockGuard() {
        lock_.UnLockRead();
    }

 private:
    FairRWLock& lock_;
};

class FairWriteLockGuard {
 public:
    explicit FairWriteLockGuard(FairRWLock& lock) : lock_(lock) {
        lock_.LockWrite();
    }

    ~FairWriteLockGuard() {
        lock_.UnLockWrite();
    }

 private:
    FairRWLock& lock_;
};
}  // namespace knowhere
#endif
