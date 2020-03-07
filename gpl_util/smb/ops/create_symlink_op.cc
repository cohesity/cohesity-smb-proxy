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

#include <glog/logging.h>
#include <string>

#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/base/util.h"
#include "gpl_util/smb/ops/create_symlink_op.h"
#include "gpl_util/smb/util.h"

using cohesity::apache_util::Error;
using cohesity::apache_util::smb::SmbProxyCreateSymlinkArg;
using cohesity::apache_util::smb::SmbProxyCreateSymlinkResult;
using std::move;
using std::string;

DECLARE_int32(smb_proxy_file_max_write_retries);

namespace cohesity { namespace gpl_util { namespace smb {

//-----------------------------------------------------------------------------

SmbProxy::CreateSymlinkOp::Ptr SmbProxy::CreateSymlinkOp::CreatePtr(
    SmbProxy* owner,
    const SmbProxyCreateSymlinkArg& arg,
    SmbProxyCreateSymlinkResult* result) {
  return Ptr(new CreateSymlinkOp(owner, arg, result));
}

//-----------------------------------------------------------------------------

SmbProxy::CreateSymlinkOp::CreateSymlinkOp(
    SmbProxy* owner,
    const SmbProxyCreateSymlinkArg& arg,
    SmbProxyCreateSymlinkResult* result)
    : owner_(owner), arg_(arg), result_(result) {
  DCHECK(owner_);
  DCHECK(result_);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateSymlinkOp::Start() {
  const auto& header = arg_.header();
  auto error = owner_->ValidateSmbProxyBaseArg(header);
  if (!error.Ok()) {
    *result_->mutable_error() = move(error);
    return;
  }

  if (arg_.target_path().empty()) {
    *result_->mutable_error() =
        Error(Error::kInvalid, "The 'target_path' field cannot be empty");
    return;
  }

  const string& path = arg_.header().root_dir() + arg_.path();
  string smb_path;

  // Get the proper SMB paths for smbc_wrapper.
  string target_symlink_path = arg_.target_path();
  // Because an SMB symlink path could contain an UNC path, which has leading
  // "\\", we should not call GetSmbPath() for this path which will remove the
  // redundant slashes.
  replace(target_symlink_path.begin(), target_symlink_path.end(), '/', '\\');
  uint32 flags = 0;
  if (target_symlink_path[0] != '\\') {
    // This is a relative symlink.
    flags = kSymlinkFlagRelativePath;
  }

  while (num_retries_ <= FLAGS_smb_proxy_file_max_write_retries) {
    auto result_pair = owner_->GetOrCreateSmbConnection(header);
    conn_ = result_pair.first;
    if (!conn_) {
      DCHECK(!result_pair.second.Ok());
      *result_->mutable_error() = move(result_pair.second);
      return;
    }

    if (smb_path.empty()) {
      smb_path = GetSmbPath(path, &conn_->context);
    }
    auto ret = smbc_wrapper_create_symlink(
        smb_path.c_str(), target_symlink_path.c_str(), &conn_->context, flags);
    if (ret == -1) {
      error = FromSmbErrorMessage(
          conn_->context.nt_status,
          StringPrintf("Failed to create symlink at %s to %s, error msg: %s",
                       path.c_str(),
                       arg_.target_path().c_str(),
                       conn_->context.error_msg));

      if (error.type() == Error::kRetry) {
        // The connection is not usable, we should retry.
        owner_->ShutdownSmbConnection(
            header.task_key(), header.server() + header.share(), conn_);
        owner_->ReleaseSmbConnection(header, move(conn_), false /* reuse */);
        ++num_retries_;
        continue;
      }

      // Non-retriable error.
      *result_->mutable_error() = move(error);
    }

    DCHECK(conn_);
    owner_->ReleaseSmbConnection(header, move(conn_));
    return;
  }

  // Cannot succeed after retries.
  DCHECK(!error.Ok());
  error.set_type(Error::kTimeout);
  *result_->mutable_error() = move(error);
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
