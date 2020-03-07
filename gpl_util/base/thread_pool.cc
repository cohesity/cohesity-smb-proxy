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
// A thread pool class to provide the functionality to execute work units
// asynchronously.
// Note: The destructor of this class takes care of cleaning up the threads but
// relies on the caller to terminate the work unit scheduled to be run by this
// thread pool. For example, if following work unit is scheduled on a thread of
// this class:
//
// http_server_ = apache_util::http_server::CreateCommonHttpServer(http_port_);
// auto work_unit = [this]() {
//   http_server_->start()
// }
//
// then it is the responsibility of the caller to finish the blocking call so
// that the thread can terminate and release its resources properly. The
// responsibility to terminate the thread is to be handled by the caller
// because there is no reliable way to terminate a running thread in C++.

#include <glog/logging.h>
#include <memory>

#include "gpl_util/base/thread_pool.h"

using cohesity::apache_util::Error;
using std::function;
using std::thread;

namespace cohesity { namespace gpl_util {

//-----------------------------------------------------------------------------

ThreadPool::ThreadPool(uint32 num_threads)
    : num_threads_(num_threads), stopped_(false) {
  sem_init(&pending_work_items_sem_,
           0 /* share only between threads of this process */,
           0 /* starting at 0 because the queue is empty */);
}

//-----------------------------------------------------------------------------

ThreadPool::~ThreadPool() {
  WaitAndCleanUp();
}

//-----------------------------------------------------------------------------

Error ThreadPool::Init() {
  // The function for worker threads to execute.
  std::function<void()> worker_thread_func = [this]() {
    while (true) {
      // Wait until the next work item is available.
      sem_wait(&pending_work_items_sem_);

      work_items_queue_lock_.lock();
      DCHECK(!work_items_queue_.empty());
      auto work_item = work_items_queue_.front();
      work_items_queue_.pop();
      work_items_queue_lock_.unlock();

      // A null work_item means that this worker thread needs to terminate.
      if (!work_item) {
        LOG(INFO) << "Terminating worker thread with id "
                  << std::this_thread::get_id();
        return;
      }

      // Run the work item.
      work_item();
    }
  };

  worker_thread_vec_lock_.lock();
  for (uint32 ii = 0; ii < num_threads_; ++ii) {
    thread worker(worker_thread_func);
    LOG(INFO) << "Creating a worker thread with id " << worker.get_id();
    worker_thread_vec_.emplace_back(move(worker));
  }
  worker_thread_vec_lock_.unlock();

  return Error();
}

//-----------------------------------------------------------------------------

Error ThreadPool::QueueWork(function<void()> work_item) {
  DCHECK(work_item);

  return QueueWorkInternal(move(work_item));
}

//-----------------------------------------------------------------------------

void ThreadPool::WaitAndCleanUp() {
  // Guard against multiple WaitAndCleanUp calls.
  if (stopped_.exchange(true)) {
    LOG(INFO) << "Threadpool cleanup is already initiated.";
    return;
  }

  worker_thread_vec_lock_.lock();
  auto active_worker_threads = worker_thread_vec_.size();
  worker_thread_vec_lock_.unlock();

  if (active_worker_threads == 0) {
    LOG(ERROR) << "The thread pool has not been initialized";
    return;
  }

  // Schedule null work units to terminate the threads.
  for (uint32 ii = 0; ii < active_worker_threads; ++ii) {
    QueueWorkInternal(nullptr);
  }

  worker_thread_vec_lock_.lock();
  for (auto& worker : worker_thread_vec_) {
    // Wait for each thread to terminate.
    LOG(INFO) << "Waiting for join on worker thread with id "
              << worker.get_id();
    worker.join();
  }
  worker_thread_vec_lock_.unlock();
}

//-----------------------------------------------------------------------------

Error ThreadPool::QueueWorkInternal(function<void()> work_item) {
  work_items_queue_lock_.lock();
  work_items_queue_.emplace(move(work_item));
  work_items_queue_lock_.unlock();

  // Notify the threads that there is a work item available.
  sem_post(&pending_work_items_sem_);

  return Error();
}

//-----------------------------------------------------------------------------

} }  // namespace cohesity, gpl_util
