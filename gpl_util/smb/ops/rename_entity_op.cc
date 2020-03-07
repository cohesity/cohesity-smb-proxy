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

#include <glog/logging.h>
#include <string>

#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/base/util.h"
#include "gpl_util/smb/ops/rename_entity_op.h"
#include "gpl_util/smb/util.h"

using cohesity::apache_util::Error;
using cohesity::apache_util::smb::SmbProxyRenameEntityArg;
using cohesity::apache_util::smb::SmbProxyRenameEntityResult;
using std::move;
using std::string;

DECLARE_int32(smb_proxy_file_max_write_retries);

namespace cohesity { namespace gpl_util { namespace smb {

//-----------------------------------------------------------------------------

SmbProxy::RenameEntityOp::Ptr SmbProxy::RenameEntityOp::CreatePtr(
    SmbProxy* owner,
    const SmbProxyRenameEntityArg& arg,
    SmbProxyRenameEntityResult* result) {
  return Ptr(new RenameEntityOp(owner, arg, result));
}

//-----------------------------------------------------------------------------

SmbProxy::RenameEntityOp::RenameEntityOp(SmbProxy* owner,
                                         const SmbProxyRenameEntityArg& arg,
                                         SmbProxyRenameEntityResult* result)
    : owner_(owner), arg_(arg), result_(result) {
  DCHECK(owner_);
  DCHECK(result_);
}

//-----------------------------------------------------------------------------

void SmbProxy::RenameEntityOp::Start() {
  const auto& header = arg_.header();
  auto error = owner_->ValidateSmbProxyBaseArg(header);
  if (!error.Ok()) {
    *result_->mutable_error() = move(error);
    return;
  }

  // Verify that the source path is non-empty.
  if (arg_.existing_path().empty()) {
    *result_->mutable_error() =
        Error(Error::kInvalid, "The 'existing_path' field cannot be empty");
    return;
  }

  // Verify that the target path is non-empty.
  if (arg_.target_path().empty()) {
    *result_->mutable_error() =
        Error(Error::kInvalid, "The 'target_path' field cannot be empty");
    return;
  }

  const string& existing_path =
      arg_.header().root_dir() + arg_.existing_path();
  const string& target_path = arg_.header().root_dir() + arg_.target_path();

  // Get the proper SMB paths for smbc_wrapper.
  string smb_existing_path;
  // smbc_wrapper_rename method does two things
  // 1. Make smb CREATE call to the server to get the FILE NUM of the source
  // file. In case of DFS capable SMB server, SMB CREATE call is DFS op. So the
  // path should be the DFS path.
  // 2. Make smb RENAME call to the server. All SMB ops other than CREATE is
  // non-DFS path and expect a non-dfs path for its args. In case of DFS
  // capable SMB server, the create call is DFS op but the rename is non-dfs
  // regular op.
  // The Args to Rename smb call is source FILE NUM and Destination
  // file name. So we don't set the dfs path for the destination path since SMB
  // rename call is non-dfs op. We set the dfs path for source file because the
  // Create call is a DFS OP. Setting connection context to nullptr to set
  // non-dfs path for target path.
  string smb_target_path = GetSmbPath(target_path, nullptr /* context */);

  // Convert entity_type to the appropriate enum for smbc_wrapper.
  smbc_entity_type entity_type = SMBC_ENTITY_TYPE_REGULAR;
  if (arg_.has_entity_type()) {
    entity_type = ConvertEntityTypeToSmb(arg_.entity_type());
  }

  while (num_retries_ <= FLAGS_smb_proxy_file_max_write_retries) {
    auto result_pair = owner_->GetOrCreateSmbConnection(header);
    conn_ = result_pair.first;
    if (!conn_) {
      DCHECK(!result_pair.second.Ok());
      *result_->mutable_error() = move(result_pair.second);
      return;
    }

    if (smb_existing_path.empty()) {
      smb_existing_path = GetSmbPath(existing_path, &conn_->context);
    }

    // If there are any open file handles to the target file, SMB server will
    // fail the rename operation with STATUS_ACCESS_DENIED when ReplaceIfExists
    // is set to true. If the caller has requested to replace the target file,
    // close any file handles for the target file that are cached by the task.
    if (arg_.replace_if_exists()) {
      owner_->PurgeFileHandle(arg_.header().task_key(), target_path);
    }

    auto ret = smbc_wrapper_rename(smb_existing_path.c_str(),
                                   smb_target_path.c_str(),
                                   entity_type,
                                   arg_.replace_if_exists(),
                                   &conn_->context);
    if (ret == -1) {
      error = FromSmbErrorMessage(
          conn_->context.nt_status,
          StringPrintf("Failed to rename entity %s to %s, error msg: %s",
                       existing_path.c_str(),
                       target_path.c_str(),
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
