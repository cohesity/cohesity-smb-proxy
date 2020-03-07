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
// This file defines a simple LRU cache.
//
// Thread-safety: This class is not thread-safe.

#ifndef _GPL_UTIL_BASE_LRU_CACHE_H_
#define _GPL_UTIL_BASE_LRU_CACHE_H_

#include <glog/logging.h>
#include <list>
#include <memory>
#include <tuple>
#include <unordered_map>

#include "apache_util/base/basictypes.h"

namespace cohesity { namespace gpl_util {

template <typename KeyType, typename ValueType>
class LRUCache {
 public:
  typedef std::shared_ptr<LRUCache> Ptr;
  typedef std::shared_ptr<const LRUCache> PtrConst;

  // Ctor. Creates an LRU cache with 'max_entries'.
  explicit LRUCache(int64 max_entries);

  // Dtor.
  ~LRUCache();

  // Adds a key and value entry to the cache. If the key already exists in the
  // cache, then the value passed replaces the existing value. The updated
  // entry is inserted as the most recently used entry.
  void Insert(const KeyType& key, const ValueType& value);

  // Finds the value associated with the given key. Return true if value is
  // found, false otherwise. If the entry exists and the pointer to 'value' is
  // not NULL, then entry will be copied to 'value'.
  bool Lookup(const KeyType& key, ValueType* value = nullptr);

  // Removes entry from the cache for the given key.
  void Remove(const KeyType& key);

  // Clears the cache.
  void Clear();

 private:
  // EntryType tuple is stored in the entry_list_ declared below. The tuple
  // contains KeyType and ValueType.
  typedef std::tuple<KeyType, ValueType> EntryType;

  // ListIteratorType is an iterator type to entry_list_.
  typedef typename std::list<EntryType>::iterator ListIteratorType;

  // List used to store the actual key and value. The back of the list
  // represents the least recently used item (candidate for eviction).
  std::list<EntryType> entry_list_;

  // The map contains mappings from key to iterators in the entry_list_.
  // The size of entry_map_ and list should always be the same.
  std::unordered_map<KeyType, ListIteratorType> entry_map_;

  // The maximum entries allowed in the cache. If we exceed this maximum value,
  // the entry at the back of the list is evicted.
  const int64 max_entries_;

 private:
  DISALLOW_COPY_AND_ASSIGN(LRUCache);
};

//-----------------------------------------------------------------------------

template <typename KeyType, typename ValueType>
LRUCache<KeyType, ValueType>::LRUCache(int64 max_entries)
    : max_entries_(max_entries) {
  CHECK_GT(max_entries_, 0);
}

//-----------------------------------------------------------------------------

template <typename KeyType, typename ValueType>
LRUCache<KeyType, ValueType>::~LRUCache() {
  Clear();
}

//-----------------------------------------------------------------------------

template <typename KeyType, typename ValueType>
void LRUCache<KeyType, ValueType>::Insert(const KeyType& key,
                                          const ValueType& value) {
  // Add an entry to the front of list.
  ListIteratorType it = entry_list_.emplace(entry_list_.begin(), key, value);

  // Update the entry in the map.
  auto map_it = entry_map_.find(key);
  if (map_it != entry_map_.end()) {
    // Remove existing entry from the list.
    entry_list_.erase(map_it->second);
    map_it->second = it;
  } else {
    CHECK(entry_map_.emplace(key, it).second);
  }
  DCHECK_EQ(entry_map_.size(), entry_list_.size());

  // Remove old entry from the end of the list if the size exceeds
  // max_entries_.
  if (entry_map_.size() > max_entries_ * 1ULL) {
    const auto& entry = entry_list_.back();
    entry_map_.erase(std::get<0>(entry));
    entry_list_.pop_back();
  }
}

//-----------------------------------------------------------------------------

template <typename KeyType, typename ValueType>
bool LRUCache<KeyType, ValueType>::Lookup(const KeyType& key,
                                          ValueType* value) {
  auto map_it = entry_map_.find(key);
  if (map_it == entry_map_.end()) {
    return false;
  }

  if (value) {
    *value = std::get<1>(*map_it->second);
  }

  // Remove it->second from entry_list_ and insert it at the beginning of the
  // entry_list_. Splice does not invalidate the iterators stored in the
  // entry_map_.
  entry_list_.splice(entry_list_.begin(), entry_list_, map_it->second);

  return true;
}

//-----------------------------------------------------------------------------

template <typename KeyType, typename ValueType>
void LRUCache<KeyType, ValueType>::Remove(const KeyType& key) {
  auto map_it = entry_map_.find(key);
  if (map_it != entry_map_.end()) {
    entry_list_.erase(map_it->second);
    entry_map_.erase(map_it);
  }
}

//-----------------------------------------------------------------------------

template <typename KeyType, typename ValueType>
void LRUCache<KeyType, ValueType>::Clear() {
  entry_map_.clear();
  entry_list_.clear();
}

//-----------------------------------------------------------------------------

} }  // namespace cohesity, gpl_util

#endif  // _GPL_UTIL_BASE_LRU_CACHE_H_
