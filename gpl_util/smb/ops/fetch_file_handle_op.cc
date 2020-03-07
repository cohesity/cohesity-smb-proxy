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

#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/smb/ops/fetch_file_handle_op.h"

#include <glog/logging.h>
#include <string>

#include "gpl_util/smb/util.h"

using ::boost::shared_lock;
using ::boost::shared_mutex;
using ::boost::unique_lock;
using ::cohesity::apache_util::Error;
using ::cohesity::apache_util::ScopedRunner;
using ::cohesity::apache_util::smb::EntityMetadata;
using ::std::make_shared;
using ::std::move;
using ::std::pair;
using ::std::string;

DEFINE_int32(smb_proxy_open_error_cache_duration_secs, 120 /* 2 mins */,
             "The amount of time (in secs) to cache any error encountered "
             "while opening a file.");

namespace cohesity { namespace gpl_util { namespace smb {

//-----------------------------------------------------------------------------

SmbProxy::FetchFileHandleOp::Ptr SmbProxy::FetchFileHandleOp::CreatePtr(
    SmbProxy* owner,
    SmbConnection::Ptr conn,
    const string& task_key,
    const string& file_path,
    bool create_parent_dir) {
  return Ptr(new FetchFileHandleOp(
      owner, move(conn), task_key, file_path, create_parent_dir));
}

//-----------------------------------------------------------------------------

SmbProxy::FetchFileHandleOp::FetchFileHandleOp(SmbProxy* owner,
                                               SmbConnection::Ptr conn,
                                               const string& task_key,
                                               const string& file_path,
                                               bool create_parent_dir)
    : owner_(owner),
      conn_(move(conn)),
      task_key_(GetSmbPath(task_key, nullptr)),
      file_path_(GetSmbPath(file_path, &conn_->context)),
      create_parent_dir_(create_parent_dir) {
  CHECK(owner_);
  CHECK(conn_);
  DCHECK(!task_key_.empty());
  DCHECK(!file_path_.empty());
}

//-----------------------------------------------------------------------------

pair<Error, bool> SmbProxy::FetchFileHandleOp::Execute() {
  // Fetch FileHandlesEntry for the given task_key.
  auto fh_entry = owner_->GetFileHandlesEntry(task_key_);

  // Fetch file handle for the given file_path in fh_entry.
  FetchFileHandle(move(fh_entry));

  return {error_, found_cached_};
}

//-----------------------------------------------------------------------------

bool SmbProxy::FetchFileHandleOp::LookupFileHandle(
    SmbProxy::FileHandlesEntry::Ptr fh_entry) {
  DCHECK(fh_entry) << task_key_;

  unique_lock<shared_mutex> lock(fh_entry->mutex);

  SmbProxy::FileHandlesEntry::FileHandleEntry file_handle;
  if (fh_entry->fh_cache.Lookup(file_path_, &file_handle)) {
    if (file_handle.error.Ok()) {
      // Found file handle without any error.
      found_cached_ = true;
      return true;
    }

    // Found an error in the file handle. Check if it is a stale error.
    if (!IsErrorStale(file_handle)) {
      error_ = file_handle.error;
      return true;
    }
  }

  return false;
}

//-----------------------------------------------------------------------------

void SmbProxy::FetchFileHandleOp::FetchFileHandle(
    SmbProxy::FileHandlesEntry::Ptr fh_entry) {
  DCHECK(fh_entry) << task_key_;

  if (LookupFileHandle(fh_entry)) {
    // Found an entry, return from here.
    return;
  }

  // Acquire an exclusive lock before opening the file.
  owner_->id_locker().AcquireExclusiveLock(file_path_);
  lock_releaser_ = make_shared<ScopedRunner>(
      [this] { owner_->id_locker().ReleaseExclusiveLock(file_path_); });

  FetchFileHandleLocked(move(fh_entry));
}

//-----------------------------------------------------------------------------

void SmbProxy::FetchFileHandleOp::FetchFileHandleLocked(
    SmbProxy::FileHandlesEntry::Ptr fh_entry) {
  DCHECK(fh_entry) << task_key_;

  // Check if file handle was added while waiting to acquire the id lock.
  if (LookupFileHandle(fh_entry)) {
    return;
  }

  if (create_parent_dir_) {
    auto error = CreateDirectoryHelper(
        GetParentDir(file_path_), &conn_->context, create_parent_dir_);
    if (!error.Ok()) {
      error_ = move(error);
      return;
    }
  }

  // Check if the given file is present.
  EntityMetadata md;
  auto error = GetEntityMetadata(file_path_, &conn_->context, &md);
  if (error.Ok()) {
    // Found the file, add the file handle to cache and return.
  } else if (error.type() != Error::kNonExistent) {
    // Encountered a different error while getting file info.
    error_ = move(error);
  } else {
    // Given file was not found. Create it and return.
    int32 ret = smbc_wrapper_create_object(
        file_path_.c_str(), &conn_->context, 1 /* is_file */);

    if (ret < 0) {
      string error_msg = "Failed to create the file " + file_path_ +
                         ", error msg: " + conn_->context.error_msg;
      LOG(ERROR) << error_msg;
      error_ = FromSmbErrorMessage(conn_->context.nt_status, error_msg);
    }
  }

  AddToLRUCache(move(fh_entry));
}

//-----------------------------------------------------------------------------

void SmbProxy::FetchFileHandleOp::AddToLRUCache(
    SmbProxy::FileHandlesEntry::Ptr fh_entry) {
  // Create file handle.
  auto file_handle = SmbProxy::FileHandlesEntry::FileHandleEntry();
  if (!error_.Ok()) {
    file_handle.error = error_;
    file_handle.error_time_usecs = GetNowInUsecs();
  }

  // Add to the cache.
  unique_lock<shared_mutex> lock(fh_entry->mutex);

  // Validate once again before adding the file handle.
  DCHECK(!fh_entry->fh_cache.Lookup(file_path_)) << file_path_;

  // Add the new handle to the cache.
  fh_entry->fh_cache.Insert(file_path_, file_handle);
}

//-----------------------------------------------------------------------------

bool SmbProxy::FetchFileHandleOp::IsErrorStale(
    const SmbProxy::FileHandlesEntry::FileHandleEntry& file_handle) {
  DCHECK(!file_handle.error.Ok());
  const int64 threshold_usecs =
      FLAGS_smb_proxy_open_error_cache_duration_secs * kNumUsecsInSec;
  return (GetNowInUsecs() - file_handle.error_time_usecs) > threshold_usecs;
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
