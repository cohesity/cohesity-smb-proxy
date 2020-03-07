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

#include <algorithm>
#include <glog/logging.h>

#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/smb/ops/delete_entity_op.h"
#include "gpl_util/smb/util.h"

using ::cohesity::apache_util::Error;
using ::cohesity::apache_util::smb::EntityMetadata;
using ::cohesity::apache_util::smb::EntityType;
using ::cohesity::apache_util::smb::SmbProxyFetchChildrenMetadataArg;
using ::cohesity::apache_util::smb::SmbProxyFetchChildrenMetadataResult;
using ::cohesity::apache_util::smb::SmbProxyBaseArg;
using ::std::list;
using ::std::make_shared;
using ::std::move;
using ::std::shared_ptr;
using ::std::string;

namespace cohesity { namespace gpl_util { namespace smb {

//-----------------------------------------------------------------------------

SmbProxy::DeleteEntityOp::Ptr SmbProxy::DeleteEntityOp::CreatePtr(
    SmbProxy* owner,
    const SmbProxyBaseArg& header,
    const string& entity_path,
    EntityType entity_type,
    bool exclusive_access,
    SmbConnection::Ptr conn) {
  return Ptr(
      new DeleteEntityOp(owner, header, entity_path, entity_type,
      exclusive_access, conn));
}

//-----------------------------------------------------------------------------

SmbProxy::DeleteEntityOp::DeleteEntityOp(SmbProxy* owner,
                                         const SmbProxyBaseArg& header,
                                         const string& entity_path,
                                         EntityType entity_type,
                                         bool exclusive_access,
                                         SmbConnection::Ptr conn)
    : owner_(owner),
      header_(header),
      entity_path_(entity_path),
      entity_type_(entity_type),
      is_exclusive_access_(exclusive_access),
      conn_(conn) {
  DCHECK(owner_);
  if (conn_) {
    is_external_connection_ = true;
  }
}

//-----------------------------------------------------------------------------

Error SmbProxy::DeleteEntityOp::Execute() {
  auto error = owner_->ValidateSmbProxyBaseArg(header_);
  if (!error.Ok()) {
    return error;
  }

  if (!is_external_connection_) {
    auto result_pair = owner_->GetOrCreateSmbConnection(header_);
    conn_ = result_pair.first;
    if (!conn_) {
      DCHECK(!result_pair.second.Ok());
      return result_pair.second;
    }
  }

  string path = GetSmbPath(entity_path_, &conn_->context);

  // TODO: Use DeleteEntity for files too. It uses backup semantics, so there
  // is no need to reset permissions to delete a file.
  if (entity_type_ == apache_util::smb::kDirectory) {
    error = DeleteDir(move(path));
  } else if (entity_type_ == apache_util::smb::kFile) {
    error = DeleteFile(move(path));
  } else {
    error = DeleteEntity(path, entity_type_, is_exclusive_access_);
  }

  if (!is_external_connection_) {
    owner_->ReleaseSmbConnection(header_, move(conn_));
  }
  return error;
}

//-----------------------------------------------------------------------------

Error SmbProxy::DeleteEntityOp::DeleteDir(const string& dir_path) {
  // Fetch children metadata of the given directory and delete each of them.
  while (true) {
    SmbProxyFetchChildrenMetadataArg arg;
    *arg.mutable_header() = header_;
    arg.mutable_header()->set_continue_on_error(true);
    arg.set_path(dir_path);
    if (dir_path_cookie_map_.find(dir_path) != dir_path_cookie_map_.end()) {
      arg.set_cookie(dir_path_cookie_map_[dir_path]);
    }

    SmbProxyFetchChildrenMetadataResult result;

    owner_->SmbProxyFetchChildrenMetadata(arg, &result);

    for (const auto& child_entity_md : result.children_metadata()) {
      if (child_entity_md.has_error()) {
        LOG(INFO) << "Ignoring the error found in child md during delete: "
                  << child_entity_md.DebugString();
        continue;
      }
      string child_path = GetSmbPath(child_entity_md.path(), &conn_->context);

      Error error;
      if (child_entity_md.type() == EntityMetadata::kDirectory) {
        // TODO(akshay): Fix possible stack-overflow due to long recursion
        // tree.
        error = DeleteDir(child_path);
      } else {
        error = DeleteFile(child_path);
      }

      if (!error.Ok()) {
        return error;
      }
    }

    if (result.has_cookie()) {
      dir_path_cookie_map_[dir_path] = result.cookie();
    } else {
      // Completed fetching all the children. Erase the cookie and break the
      // loop.
      dir_path_cookie_map_.erase(dir_path);
      break;
    }
  }

  return DeleteEmptyDir(dir_path);
}

//-----------------------------------------------------------------------------

Error SmbProxy::DeleteEntityOp::DeleteEmptyDir(const string& dir_path) {
  int32 return_code =
      smbc_wrapper_delete_directory(dir_path.c_str(), &conn_->context);

  if (return_code == -1) {
    return FromSmbErrorMessage(conn_->context.nt_status,
                               "Failed to delete an empty directory: " +
                                   dir_path + " , error msg: " +
                                   conn_->context.error_msg);
  }
  return Error();
}

//-----------------------------------------------------------------------------

Error SmbProxy::DeleteEntityOp::DeleteFile(const string& file_path) {
  auto error = DeleteFileHelper(file_path);

  // Check if this call failed due to permission issue.
  if (!error.Ok() && error.type() == Error::kCannotDelete) {
    auto clear_attr_error = ClearFileAttributes(file_path);
    if (!clear_attr_error.Ok()) {
      LOG(ERROR) << "Failed to clear attributes of " << file_path
                 << " with error: " << clear_attr_error.DebugString();
      return error;
    }

    // Retry file deletion now.
    return DeleteFileHelper(file_path);
  }

  return error;
}

//-----------------------------------------------------------------------------

Error SmbProxy::DeleteEntityOp::DeleteFileHelper(const string& file_path) {
  int32 return_code =
      smbc_wrapper_delete_object(file_path.c_str(), &conn_->context);

  if (return_code == -1) {
    return FromSmbErrorMessage(
        conn_->context.nt_status,
        "Failed to delete an entity: " + file_path +
            " , error msg: " + conn_->context.error_msg);
  }
  return Error();
}

//-----------------------------------------------------------------------------

Error SmbProxy::DeleteEntityOp::ClearFileAttributes(const string& file_path) {
  smbc_wrapper_entity_metadata smb_metadata = {0};
  smb_metadata.attributes = kSmbAttrNormal;

  auto ret = smbc_wrapper_set_metadata(file_path.c_str(),
                                       &conn_->context,
                                       true /* set_file_info */,
                                       false /* set_acl_info */,
                                       0 /* additional_info */,
                                       &smb_metadata);
  if (ret != 0) {
    return FromSmbErrorMessage(conn_->context.nt_status,
                               "Failed to set metadata for: " + file_path +
                                   ", error msg: " + conn_->context.error_msg);
  }

  return Error();
}

//-----------------------------------------------------------------------------

Error SmbProxy::DeleteEntityOp::DeleteEntity(const string& path,
                                             EntityType entity_type,
                                             bool is_exclusive = false) {
  // Convert entity_type to the appropriate enum for smbc_wrapper.
  smbc_entity_type smb_entity_type = ConvertEntityTypeToSmb(entity_type);

  int32 return_code = smbc_wrapper_delete_entity(
      path.c_str(), smb_entity_type, &conn_->context, is_exclusive);

  if (return_code == -1) {
    return FromSmbErrorMessage(
        conn_->context.nt_status,
        "Failed to delete an entity: " + path +
            " , error msg: " + conn_->context.error_msg);
  }

  return Error();
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
