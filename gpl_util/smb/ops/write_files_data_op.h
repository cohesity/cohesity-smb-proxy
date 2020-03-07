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
// This file defines an op for writing data to multiple files on proxy server.

#ifndef _GPL_UTIL_SMB_OPS_WRITE_FILES_DATA_OP_H_
#define _GPL_UTIL_SMB_OPS_WRITE_FILES_DATA_OP_H_

#include <memory>

#include "gpl_util/smb/smb_proxy.h"

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy::WriteFilesDataOp {
 public:
  typedef std::shared_ptr<WriteFilesDataOp> Ptr;
  typedef std::shared_ptr<const WriteFilesDataOp> PtrConst;

  // Creates a WriteFilesDataOp instance.
  static Ptr CreatePtr(SmbProxy* owner,
                       const apache_util::smb::SmbProxyWriteFilesDataArg& arg,
                       apache_util::smb::SmbProxyWriteFilesDataResult* result);

  // Starts the op.
  void Start();

 private:
  // Constructor.
  WriteFilesDataOp(SmbProxy* owner,
                   const apache_util::smb::SmbProxyWriteFilesDataArg& arg,
                   apache_util::smb::SmbProxyWriteFilesDataResult* result);

  // Helper method to handle write data of one file entity.
  void HandleNextEntity();

 private:
  // The owning proxy instance.
  SmbProxy* owner_;

  // Pointer to SMB connection.
  SmbProxy::SmbConnection::Ptr conn_;

  // SmbProxyWriteFilesDataArg.
  const apache_util::smb::SmbProxyWriteFilesDataArg& arg_;

  // SmbProxyWriteFilesDataResult.
  apache_util::smb::SmbProxyWriteFilesDataResult* result_;

  // Index of next entity to be handled.
  int32 index_ = -1;

 private:
  DISALLOW_COPY_AND_ASSIGN(WriteFilesDataOp);
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_OPS_WRITE_FILES_DATA_OP_H_
