// Copyright 2019 Cohesity Inc.
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
// Author: Raghavendra Maddipatla
//
// This file defines an op for renaming an entity.

#ifndef _GPL_UTIL_SMB_OPS_RENAME_ENTITY_OP_H_
#define _GPL_UTIL_SMB_OPS_RENAME_ENTITY_OP_H_

#include <memory>

#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::RenameEntityOp {
 public:
  typedef std::shared_ptr<RenameEntityOp> Ptr;
  typedef std::shared_ptr<const RenameEntityOp> PtrConst;

  // Creates a RenameEntityOp instance.
  static Ptr CreatePtr(SmbProxy* owner,
                       const apache_util::smb::SmbProxyRenameEntityArg& arg,
                       apache_util::smb::SmbProxyRenameEntityResult* result);

  // Starts the op.
  void Start();

 private:
  // Constructor.
  RenameEntityOp(SmbProxy* owner,
                 const apache_util::smb::SmbProxyRenameEntityArg& arg,
                 apache_util::smb::SmbProxyRenameEntityResult* result);

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // Pointer to SMB connection.
  SmbProxy::SmbConnection::Ptr conn_;

  // SmbProxyRenameEntityArg.
  const apache_util::smb::SmbProxyRenameEntityArg& arg_;

  // SmbProxyRenameEntityResult.
  apache_util::smb::SmbProxyRenameEntityResult* result_;

  // The number of retries that have been done so far for this op.
  int32 num_retries_ = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(RenameEntityOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_RENAME_ENTITY_OP_H_
