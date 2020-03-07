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
// This file defines an op to create files which need to be restored.

#ifndef _GPL_UTIL_SMB_OPS_CREATE_RESTORE_FILES_OP_H_
#define _GPL_UTIL_SMB_OPS_CREATE_RESTORE_FILES_OP_H_

#include <memory>

#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::CreateRestoreFilesOp {
 public:
  typedef std::shared_ptr<CreateRestoreFilesOp> Ptr;
  typedef std::shared_ptr<const CreateRestoreFilesOp> PtrConst;

  // Creates a CreateRestoreFilesOp instance.
  static Ptr CreatePtr(
      SmbProxy* owner,
      const apache_util::smb::SmbProxyCreateRestoreFilesArg& arg,
      apache_util::smb::SmbProxyCreateRestoreFilesResult* result);

  // Starts CreateRestoreFilesOp.
  void Start();

 private:
  // Constructor.
  CreateRestoreFilesOp(
      SmbProxy* owner,
      const apache_util::smb::SmbProxyCreateRestoreFilesArg& arg,
      apache_util::smb::SmbProxyCreateRestoreFilesResult* result);

  // Helper method to handle the next entity creation.
  void HandleNextEntity();

  // Helper method to handle the entity creation.
  void HandleEntity(int32 index);

  // Helper method which will delete any temporary entry and actual entry
  // created by previous restore operation. Once the entry is deleted we
  // will recreate the entry.
  void DeleteAndCreateEntity(int32 index);

  // Helper method to delete the temporary entry created by earlier restore
  // operation.
  void DeleteTmpEntity(int32 index);

  // Helper method to delete the entry created by earlier restore operation.
  void DeleteNewEntity(int32 index);

  // Helper method to delete the existing entity (this entity is not created by
  // restore operation). This entity already exist on destination server.
  void DeleteExistingEntity(int32 index);

  // Helper method to create the entry.
  void CreateEntity(int32 index);

  // Helper method to create a file or a symlink entity.
  void DoCreateEntity(int32 index);

  // Helper method to create regular file.
  void CreateRegularFile(int32 index);

  // Helper method to create the hardlink file. In volume restore, we create
  // a tmp file for zero size hardlinks as well and the tmp path is in a
  // tmp directory created within root dir (inside the mounted volume).
  void CreateHardlink(int32 index, bool is_vol_restore = false);

  // Helper method to create soft link.
  void CreateSymlink(int32 index);

  // Check if the link and target have same inode id and therefore they are
  // hardlinks to each other.
  bool IsHardlinksToEachOther(const std::string& target,
                              const std::string& link);

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // Pointer to SMB connection.
  SmbProxy::SmbConnection::Ptr conn_;

  // SmbProxyCreateRestoreFilesArg.
  const apache_util::smb::SmbProxyCreateRestoreFilesArg& arg_;

  // SmbProxyCreateRestoreFilesResult.
  apache_util::smb::SmbProxyCreateRestoreFilesResult* result_;

  // Index of next entity to be handled.
  int32 index_ = -1;

 private:
  DISALLOW_COPY_AND_ASSIGN(CreateRestoreFilesOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_CREATE_RESTORE_FILES_OP_H_
