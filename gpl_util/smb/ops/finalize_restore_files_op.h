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
// This file defines an op that finalizes restored files.

#ifndef _GPL_UTIL_SMB_OPS_FINALIZE_RESTORE_FILES_OP_H_
#define _GPL_UTIL_SMB_OPS_FINALIZE_RESTORE_FILES_OP_H_

#include <memory>
#include <string>

#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::FinalizeRestoreFilesOp {
 public:
  typedef std::shared_ptr<FinalizeRestoreFilesOp> Ptr;
  typedef std::shared_ptr<const FinalizeRestoreFilesOp> PtrConst;

  // Creates a FinalizeRestoreFilesOp instance.
  static Ptr CreatePtr(
      SmbProxy* owner,
      const apache_util::smb::SmbProxyFinalizeRestoreFilesArg& arg,
      apache_util::smb::SmbProxyFinalizeRestoreFilesResult* result);

  // Starts the op.
  void Start();

 private:
  // Constuctor.
  FinalizeRestoreFilesOp(
      SmbProxy* owner,
      const apache_util::smb::SmbProxyFinalizeRestoreFilesArg& arg,
      apache_util::smb::SmbProxyFinalizeRestoreFilesResult* result);

  // Helper method to handle the next entity creation.
  void HandleNextEntity();

  // Helper method to rename the entity at given 'index'.
  void RenameEntity(int32 index);

  // Helper method to set the entity attribute at given 'index'.
  void SetEntityAttribute(int32 index);

  // Returns true if it is an uptiering job.
  bool IsUptierTask();

  // Get the migration symlink target path for the given file_path.
  std::string GetSymlinkTargetPath(const std::string& file_path);

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // Pointer to SMB connection.
  SmbProxy::SmbConnection::Ptr conn_;

  // SmbProxyFinalizeRestoreFilesArg.
  const apache_util::smb::SmbProxyFinalizeRestoreFilesArg& arg_;

  // SmbProxyFinalizeRestoreFilesResult.
  apache_util::smb::SmbProxyFinalizeRestoreFilesResult* result_;

  // Index of next entity to be handled.
  int32 index_ = -1;

 private:
  DISALLOW_COPY_AND_ASSIGN(FinalizeRestoreFilesOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_FINALIZE_RESTORE_FILES_OP_H_
