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
// This file defines an op to delete entity.

#ifndef _GPL_UTIL_SMB_OPS_DELETE_ENTITY_OP_H_
#define _GPL_UTIL_SMB_OPS_DELETE_ENTITY_OP_H_

#include <list>
#include <memory>
#include <string>
#include <unordered_map>

#include "apache_util/base/error.h"
#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::DeleteEntityOp {
 public:
  typedef std::shared_ptr<DeleteEntityOp> Ptr;
  typedef std::shared_ptr<const DeleteEntityOp> PtrConst;

  // Creates a DeleteEntityOp instance.
  static Ptr CreatePtr(SmbProxy* owner,
                       const apache_util::smb::SmbProxyBaseArg& header,
                       const std::string& entity_path,
                       apache_util::smb::EntityType entity_type,
                       bool exclusive_access = false,
                       SmbConnection::Ptr conn = nullptr);

  // Runs the op and returns the encountered error.
  apache_util::Error Execute();

 private:
  // Constructor.
  DeleteEntityOp(SmbProxy* owner,
                 const apache_util::smb::SmbProxyBaseArg& header,
                 const std::string& entity_path,
                 apache_util::smb::EntityType entity_type,
                 bool exclusive_access,
                 SmbConnection::Ptr conn);

  // Helper method to delete a directory. If the directory is not empty, it
  // deletes all the children entities first, and then deletes the directory.
  // On success, it returns an error of kNoError type. On failure, it returns
  // appropriate error.
  apache_util::Error DeleteDir(const std::string& dir_path);

  // Helper method to delete an empty directory.
  apache_util::Error DeleteEmptyDir(const std::string& dir_path);

  // Helper method to delete a single entity (file or empty directory or
  // symlink).
  apache_util::Error DeleteEntity(const std::string& path,
                                  apache_util::smb::EntityType entity_type,
                                  bool is_exclusive);

  // A method to delete a file. It returns kNoError type error on success, and
  // appropriate error on failure.
  // This function calls DeleteFileHelper to delete the given file. If the call
  // fails with Error::kCannotDelete error, this function clears the file
  // attributes and calls DeleteFileHelper again.
  apache_util::Error DeleteFile(const std::string& file_path);

  // A helper method to delete a file.
  apache_util::Error DeleteFileHelper(const std::string& file_path);

  // A helper method to clear SMB file attributes of a given file.
  apache_util::Error ClearFileAttributes(const std::string& file_path);

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // SmbProxyBaseArg representing the task that sent the request.
  const apache_util::smb::SmbProxyBaseArg& header_;

  // The absolute path of the file or directory which needs to be deleted.
  const std::string& entity_path_;

  // Type of the entity.
  apache_util::smb::EntityType entity_type_;

  // Indicates if delete is in exlusive access mode
  bool is_exclusive_access_ = false;

  // Pointer to SMB connection.
  SmbProxy::SmbConnection::Ptr conn_;

  // Indicates if the Smb connection is passed while creating the op.
  bool is_external_connection_ = false;

  // An unordered map of directory path to cookie. These cookies are used to
  // fetch children of large directories.
  std::unordered_map<std::string, std::string> dir_path_cookie_map_;

 private:
  DISALLOW_COPY_AND_ASSIGN(DeleteEntityOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_DELETE_ENTITY_OP_H_
