// Copyright 2018 Cohesity Inc.
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
// Author: Akshay Hebbar Yedagere Sudharshana (akshay@cohesity.com)

#include "gpl_util/base/id_locker.h"

#include <glog/logging.h>
#include <string>

using ::boost::shared_lock;
using ::boost::shared_mutex;
using ::boost::unique_lock;
using ::std::string;

namespace cohesity { namespace gpl_util {

//-----------------------------------------------------------------------------

template <typename IDType>
IDLocker<IDType>::IDLocker() {}

//-----------------------------------------------------------------------------

template <typename IDType>
IDLocker<IDType>::~IDLocker() {
  shared_lock<shared_mutex> lock(mutex_);
  if (!id_map_.empty()) {
    LOG(ERROR) << "IDLocker object is being destroyed when the following "
                  "locks or waiters are outstanding";
    for (const auto& it : id_map_) {
      LOG(ERROR) << it.first << ": " << it.second.ToString();
    }
  }
}

//-----------------------------------------------------------------------------

template <typename IDType>
void IDLocker<IDType>::AcquireSharedLock(const IDType& id) {
  unique_lock<shared_mutex> g_lock(mutex_);

  IDState& id_state = id_map_[id];
  // Increase the reader_count before releasing global lock.
  const auto reader_count = ++id_state.reader_count;
  DCHECK_GT(reader_count, 0) << id;

  // Release the global lock before trying to acquire the shared lock. This is
  // because the below call could be a blocking call.
  g_lock.unlock();

  id_state.mutex.lock_shared();
}

//-----------------------------------------------------------------------------

template <typename IDType>
void IDLocker<IDType>::ReleaseSharedLock(const IDType& id) {
  unique_lock<shared_mutex> g_lock(mutex_);

  IDState& id_state = id_map_[id];
  // Decrease the reader_count.
  const auto reader_count = --id_state.reader_count;
  DCHECK_GE(reader_count, 0) << id;

  // Release the local shared lock.
  id_state.mutex.unlock_shared();

  // Try to cleanup the state if counters are zero.
  MaybeCleanupIDStateLocked(id, id_state);
}

//-----------------------------------------------------------------------------

template <typename IDType>
void IDLocker<IDType>::AcquireExclusiveLock(const IDType& id) {
  unique_lock<shared_mutex> g_lock(mutex_);

  IDState& id_state = id_map_[id];
  // Increase the writer_count before releasing global lock.
  const auto writer_count = ++id_state.writer_count;
  DCHECK_GT(writer_count, 0) << id;

  // Release the global lock before trying to acquire the exclusive lock. This
  // is because the below call could be a blocking call.
  g_lock.unlock();

  id_state.mutex.lock();
}

//-----------------------------------------------------------------------------

template <typename IDType>
void IDLocker<IDType>::ReleaseExclusiveLock(const IDType& id) {
  unique_lock<shared_mutex> g_lock(mutex_);

  IDState& id_state = id_map_[id];
  // Decrease the writer_count.
  const auto writer_count = --id_state.writer_count;
  DCHECK_GE(writer_count, 0) << id;

  // Release the local exclusive lock.
  id_state.mutex.unlock();

  // Try to cleanup the state if counters are zero.
  MaybeCleanupIDStateLocked(id, id_state);
}

//-----------------------------------------------------------------------------

template <typename IDType>
void IDLocker<IDType>::MaybeCleanupIDStateLocked(const IDType& id,
                                                 const IDState& id_state) {
  if (id_state.reader_count == 0 && id_state.writer_count == 0) {
    id_map_.erase(id);
  }
}

//-----------------------------------------------------------------------------

// Instantiations of IDLocker class for a few types.
template class IDLocker<string>;
template class IDLocker<int32>;

//-----------------------------------------------------------------------------

} }  // namespace cohesity, gpl_util
