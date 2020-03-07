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
//
// This file defines a simple locker based on different types of keys.
//
// Thread-safety: All methods of this class are thread-safe.

#ifndef _GPL_UTIL_BASE_ID_LOCKER_H_
#define _GPL_UTIL_BASE_ID_LOCKER_H_

#include <boost/thread/shared_mutex.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "apache_util/base/basictypes.h"

namespace cohesity { namespace gpl_util {

template <typename IDType>
class IDLocker {
 public:
  typedef std::shared_ptr<IDLocker> Ptr;
  typedef std::shared_ptr<const IDLocker> PtrConst;

  // Ctor.
  IDLocker();

  // Dtor.
  ~IDLocker();

  // Acquires a shared lock on the given 'id'.
  void AcquireSharedLock(const IDType& id);

  // Releases the shared lock on the given 'id'.
  void ReleaseSharedLock(const IDType& id);

  // Acquires an exclusive lock on the given 'id'.
  void AcquireExclusiveLock(const IDType& id);

  // Releases the exclusive lock on the given 'id'.
  void ReleaseExclusiveLock(const IDType& id);

 private:
  // State maintained for each id which is currently in use.
  struct IDState {
    // A counter to keep track of waiting/ongoing readers.
    int32 reader_count = 0;

    // A counter to keep track of waiting/ongoing writers.
    int32 writer_count = 0;

    // Mutex for protecting the resource.
    boost::shared_mutex mutex;

    // A helper method to return counts.
    std::string ToString() const {
      return "reader_count: " + std::to_string(reader_count) +
             ", writer_count: " + std::to_string(writer_count);
    }
  };

  // A helper method to remove the given 'id' from the 'id_map_' if counters of
  // 'id_state' have reached zero. This method should be called after acquiring
  // 'mutex_'.
  void MaybeCleanupIDStateLocked(const IDType& id, const IDState& id_state);

 private:
  // A mutex to protect 'id_map_'.
  boost::shared_mutex mutex_;

  // A map of IDs currently locked.
  std::unordered_map<IDType, IDState> id_map_;

 private:
  DISALLOW_COPY_AND_ASSIGN(IDLocker);
};

} }  // namespace cohesity, gpl_util

#endif  // _GPL_UTIL_BASE_ID_LOCKER_H_
