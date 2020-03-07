// Copyright 2017 Cohesity Inc.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//
// Author: Zheng Cai
//
// This file defines a basic thread pool which runs queued work in multiple
// threads.

#ifndef _GPL_UTIL_BASE_THREAD_POOL_H_
#define _GPL_UTIL_BASE_THREAD_POOL_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <semaphore.h>
#include <thread>
#include <vector>

#include "apache_util/base/basictypes.h"
#include "apache_util/base/error.h"

namespace cohesity { namespace gpl_util {

class ThreadPool {
 public:
  // Convenient typedef.
  using UniquePtr = std::unique_ptr<ThreadPool>;

  // Construct a ThreadPool object that contains the specified number of
  // threads. This does not actually create the underlying thread pool. The
  // caller must call Init() before using the thread pool.
  explicit ThreadPool(uint32 num_threads);

  ~ThreadPool();

  // Initializes the underlying thread pool.
  apache_util::Error Init();

  // Queues the given work item onto the thread pool. The work_item cannot be
  // null.
  apache_util::Error QueueWork(std::function<void()> work_item);

  // Waits until all outstanding work items are done, then cleans up.
  void WaitAndCleanUp();

  // Returns the number of threads in this thread pool.
  inline uint32 num_threads() const {
    return num_threads_;
  }

 private:
  // Queues the given work item onto the thread pool. This allows the work_item
  // to be null, which indicates that the next worker thread should wake up and
  // terminate.
  apache_util::Error QueueWorkInternal(std::function<void()> work_item);

 private:
  // The number of threads in the thread pool.
  const uint32 num_threads_;

  // The semaphore for notifying worker threads about new work items.
  sem_t pending_work_items_sem_;

  // The vector of worker threads.
  std::vector<std::thread> worker_thread_vec_;

  // The lock to protect member variable worker_thread_vec_.
  std::mutex worker_thread_vec_lock_;

  // The lock to protect member variable work_items_queue_.
  std::mutex work_items_queue_lock_;

  // The queue for incoming work items.
  std::queue<std::function<void()>> work_items_queue_;

  // Indicates if thread pool has been stopped.
  std::atomic_bool stopped_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ThreadPool);
};

} }  // namespace cohesity, gpl_util

#endif  // _GPL_UTIL_BASE_THREAD_POOL_H_
