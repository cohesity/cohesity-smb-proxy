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
// This file defines an op that fetches cached file handle for a given task and
// file.

#ifndef _GPL_UTIL_SMB_OPS_FETCH_FILE_HANDLE_OP_H_
#define _GPL_UTIL_SMB_OPS_FETCH_FILE_HANDLE_OP_H_

#include <memory>
#include <string>
#include <utility>

#include "apache_util/base/basictypes.h"
#include "apache_util/base/scoped_runner.h"
#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::FetchFileHandleOp {
 public:
  typedef std::shared_ptr<FetchFileHandleOp> Ptr;
  typedef std::shared_ptr<const FetchFileHandleOp> PtrConst;

  // Creates a FetchFileHandleOp instance.
  static Ptr CreatePtr(SmbProxy* owner,
                       SmbConnection::Ptr conn,
                       const std::string& task_key,
                       const std::string& file_path,
                       bool create_parent_dir = false);

  // Executes FetchFileHandleOp, and returns a pair of error and boolean
  // indicating the error occured during the Op and whether the file-handle was
  // found in the cache.
  std::pair<apache_util::Error, bool> Execute();

 private:
  // Constructor.
  FetchFileHandleOp(SmbProxy* owner,
                    SmbConnection::Ptr conn,
                    const std::string& task_key,
                    const std::string& file_path,
                    bool create_parent_dir = false);

  // Looks up the file handle in the given 'fh_entry' and sets 'found_cached_'
  // and 'error_' if it finds file handle of 'file_path_'.
  bool LookupFileHandle(SmbProxy::FileHandlesEntry::Ptr fh_entry);

  // Fetches file handle of 'file_path_'.
  void FetchFileHandle(SmbProxy::FileHandlesEntry::Ptr fh_entry);

  // Called after acquiring id lock on the 'file_path_'. If the 'file_path_' is
  // not present, then a new file is created.
  void FetchFileHandleLocked(SmbProxy::FileHandlesEntry::Ptr fh_entry);

  // A helper function to add the 'file_path_' into the 'fh_entry's cache.
  void AddToLRUCache(SmbProxy::FileHandlesEntry::Ptr fh_entry);

  // A helper function to determine if the given 'file_handle' has stale error.
  bool IsErrorStale(
      const SmbProxy::FileHandlesEntry::FileHandleEntry& file_handle);

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // Pointer to SMB connection.
  SmbProxy::SmbConnection::Ptr conn_;

  // A unique key representing the task that sent the request.
  const std::string task_key_;

  // The absolute path of the file whose handle is being fetched.
  const std::string file_path_;

  // Whether to create the parent directory.
  const bool create_parent_dir_;

  // The error occured during the op execution.
  apache_util::Error error_;

  // Whether the file was found in cache and was not newly opened.
  bool found_cached_ = false;

  // A reference-counted scoped runner to release the lock held on file_path_.
  std::shared_ptr<apache_util::ScopedRunner> lock_releaser_;

 private:
  DISALLOW_COPY_AND_ASSIGN(FetchFileHandleOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_FETCH_FILE_HANDLE_OP_H_
