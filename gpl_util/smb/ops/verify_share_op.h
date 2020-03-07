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
// Author: Zheng Cai
//
// This file defines an op for verifying a SMB share can be accessed.

#ifndef _GPL_UTIL_SMB_OPS_VERIFY_SHARE_OP_H_
#define _GPL_UTIL_SMB_OPS_VERIFY_SHARE_OP_H_

#include <memory>

#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::VerifyShareOp {
 public:
  typedef std::shared_ptr<VerifyShareOp> Ptr;
  typedef std::shared_ptr<const VerifyShareOp> PtrConst;

  // Creates a VerifyShareOp instance.
  static Ptr CreatePtr(SmbProxy* owner,
                       const apache_util::smb::SmbProxyVerifyShareArg& arg,
                       apache_util::smb::SmbProxyVerifyShareResult* result);

  // Starts the op.
  void Start();

 private:
  // Constructor.
  VerifyShareOp(SmbProxy* owner,
                const apache_util::smb::SmbProxyVerifyShareArg& arg,
                apache_util::smb::SmbProxyVerifyShareResult* result);

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // SmbProxyVerifyShareArg.
  const apache_util::smb::SmbProxyVerifyShareArg& arg_;

  // SmbProxyVerifyShareResult.
  apache_util::smb::SmbProxyVerifyShareResult* result_;

 private:
  DISALLOW_COPY_AND_ASSIGN(VerifyShareOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_VERIFY_SHARE_OP_H_
