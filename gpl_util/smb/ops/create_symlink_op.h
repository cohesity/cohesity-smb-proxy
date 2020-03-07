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
// Author: Zheng Cai
//
// This file defines an op for creating an SMB symlink (reparse point).

#ifndef _GPL_UTIL_SMB_OPS_CREATE_SYMLINK_OP_H_
#define _GPL_UTIL_SMB_OPS_CREATE_SYMLINK_OP_H_

#include <memory>

#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::CreateSymlinkOp {
 public:
  typedef std::shared_ptr<CreateSymlinkOp> Ptr;
  typedef std::shared_ptr<const CreateSymlinkOp> PtrConst;

  // Creates a CreateSymlinkOp instance.
  static Ptr CreatePtr(SmbProxy* owner,
                       const apache_util::smb::SmbProxyCreateSymlinkArg& arg,
                       apache_util::smb::SmbProxyCreateSymlinkResult* result);

  // Starts the op.
  void Start();

 private:
  // Constructor.
  CreateSymlinkOp(SmbProxy* owner,
                  const apache_util::smb::SmbProxyCreateSymlinkArg& arg,
                  apache_util::smb::SmbProxyCreateSymlinkResult* result);

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // Pointer to SMB connection.
  SmbProxy::SmbConnection::Ptr conn_;

  // SmbProxyCreateSymlinkArg.
  const apache_util::smb::SmbProxyCreateSymlinkArg& arg_;

  // SmbProxyCreateSymlinkResult.
  apache_util::smb::SmbProxyCreateSymlinkResult* result_;

  // The number of retries that have been done so far for this op.
  int32 num_retries_ = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(CreateSymlinkOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_CREATE_SYMLINK_OP_H_
