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

#include <glog/logging.h>

#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/smb/ops/write_files_data_op.h"
#include "gpl_util/smb/util.h"

using cohesity::apache_util::Error;
using cohesity::apache_util::smb::SmbProxyWriteFilesDataArg;
using cohesity::apache_util::smb::SmbProxyWriteFilesDataResult;
using std::move;

namespace cohesity { namespace gpl_util { namespace smb {

//-----------------------------------------------------------------------------

SmbProxy::WriteFilesDataOp::Ptr SmbProxy::WriteFilesDataOp::CreatePtr(
    SmbProxy* owner,
    const SmbProxyWriteFilesDataArg& arg,
    SmbProxyWriteFilesDataResult* result) {
  return Ptr(new WriteFilesDataOp(owner, arg, result));
}

//-----------------------------------------------------------------------------

SmbProxy::WriteFilesDataOp::WriteFilesDataOp(
    SmbProxy* owner,
    const SmbProxyWriteFilesDataArg& arg,
    SmbProxyWriteFilesDataResult* result)
    : owner_(owner), arg_(arg), result_(result) {
  DCHECK(owner_);
  DCHECK(result_);
}

//-----------------------------------------------------------------------------

void SmbProxy::WriteFilesDataOp::Start() {
  const auto& header = arg_.header();
  auto error = owner_->ValidateSmbProxyBaseArg(header);
  if (!error.Ok()) {
    *result_->mutable_error() = move(error);
    return;
  }

  // TODO(akshay): Consider creating multiple connections to handle the
  // entities in parallel.
  auto result_pair = owner_->GetOrCreateSmbConnection(header);
  conn_ = result_pair.first;
  if (!conn_) {
    DCHECK(!result_pair.second.Ok());
    *result_->mutable_error() = result_pair.second;
    return;
  }

  // A boolean to track if the connection needs to be shutdown.
  bool shutdown_connection = false;
  Error retry_error;

  for (int32 idx = 0; idx < arg_.entities_vec_size(); ++idx) {
    // If 'shutdown_connection' is true, then all the subsequent requests on
    // this connection will fail with kRetry error. So setting the error
    // manually to avoid smb client calls.
    if (shutdown_connection) {
      *result_->add_error_vec() = retry_error;
      continue;
    }

    *result_->add_error_vec() = Error();
    HandleNextEntity();
    if (result_->error_vec(idx).type() != Error::kNoError) {
      if (!result_->has_error()) {
        *result_->mutable_error() = result_->error_vec(idx);
      }
      // If the error is kRetry, it means that it is a disconnected
      // connection, in which case we need to shut down the connection purge
      // it from the cache.
      if (result_->error_vec(idx).type() == Error::kRetry) {
        shutdown_connection = true;
        retry_error = result_->error_vec(idx);
      }
    }
  }

  if (shutdown_connection) {
    owner_->ShutdownSmbConnection(
        header.task_key(), header.server() + header.share(), conn_);
    owner_->ReleaseSmbConnection(header, move(conn_), false /* reuse */);
  } else {
    owner_->ReleaseSmbConnection(header, move(conn_));
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::WriteFilesDataOp::HandleNextEntity() {
  const int32 index = ++index_;
  if (index >= arg_.entities_vec_size()) {
    return;
  }

  const auto& entity = arg_.entities_vec(index);
  DCHECK(entity.has_restore_file_handle()) << entity.ShortDebugString();
  const auto& file_handle = entity.restore_file_handle();

  auto offset = entity.offset();
  const auto& data = entity.data();

  // TODO(akshay): Enable support to cache_handle for writing data.
  bool cache_handle = false;

  // Try to use the temp file's path if available.
  auto path = file_handle.tmp_file_path();
  if (path.empty()) {
    path = file_handle.file_path();
  }
  CHECK(!path.empty()) << file_handle.ShortDebugString();

  auto error = owner_->WriteFileDataHelper(
      path, conn_, data, data.size(), offset, cache_handle);

  if (!error.Ok()) {
    *result_->mutable_error_vec(index) = move(error);
  }
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
