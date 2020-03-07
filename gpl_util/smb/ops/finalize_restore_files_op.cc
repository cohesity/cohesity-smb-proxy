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

#include <functional>
#include <glog/logging.h>
#include <samba/libsmbclient.h>
#include <string>

#include "apache_util/base/scoped_runner.h"
#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/smb/ops/delete_entity_op.h"
#include "gpl_util/smb/ops/finalize_restore_files_op.h"
#include "gpl_util/smb/util.h"

using ::cohesity::apache_util::Error;
using ::cohesity::apache_util::ScopedRunner;
using ::cohesity::apache_util::smb::EntityMetadata;
using ::cohesity::apache_util::smb::SmbProxyFinalizeRestoreFilesArg;
using ::cohesity::apache_util::smb::SmbProxyFinalizeRestoreFilesResult;
using ::std::function;
using ::std::move;
using ::std::string;

namespace cohesity { namespace gpl_util { namespace smb {

namespace {

//-----------------------------------------------------------------------------

// A helper function to fill smbc_wrapper_entity_metadata object from the given
// entity metadata object.
void FillSmbMetadata(smbc_wrapper_entity_metadata* smb_md,
                     const EntityMetadata& md) {
  smb_md->inode_id = md.inode_id();
  smb_md->num_hardlinks = md.num_hardlinks();
  smb_md->size = md.size();
  smb_md->allocation_size = md.allocation_size();
  smb_md->attributes = md.attributes();
  smb_md->uid = md.uid();
  smb_md->gid = md.gid();

  FillTimespecFromUsecs(&smb_md->create_time_ts, md.create_time_usecs());
  FillTimespecFromUsecs(&smb_md->access_time_ts, GetNowInUsecs());
  FillTimespecFromUsecs(&smb_md->modify_time_ts, md.modify_time_usecs());
  FillTimespecFromUsecs(&smb_md->change_time_ts, md.change_time_usecs());

  for (auto& ea : md.extended_attributes()) {
    if (ea.name() == kSmbXattrACL) {
      const string& acls = ea.value();
      smb_md->acls =
          reinterpret_cast<uint8_t*>(malloc(sizeof(uint8_t) * acls.size()));
      memcpy(smb_md->acls, acls.c_str(), acls.size());
      smb_md->acls_size = acls.size();
      break;
    }
  }
}

}  // namespace

//-----------------------------------------------------------------------------

SmbProxy::FinalizeRestoreFilesOp::Ptr
SmbProxy::FinalizeRestoreFilesOp::CreatePtr(
    SmbProxy* owner,
    const SmbProxyFinalizeRestoreFilesArg& arg,
    SmbProxyFinalizeRestoreFilesResult* result) {
  return Ptr(new FinalizeRestoreFilesOp(owner, arg, result));
}

//-----------------------------------------------------------------------------

SmbProxy::FinalizeRestoreFilesOp::FinalizeRestoreFilesOp(
    SmbProxy* owner,
    const SmbProxyFinalizeRestoreFilesArg& arg,
    SmbProxyFinalizeRestoreFilesResult* result)
    : owner_(owner), arg_(arg), result_(result) {
  DCHECK(owner_);
  DCHECK(result_);
}

//-----------------------------------------------------------------------------

void SmbProxy::FinalizeRestoreFilesOp::Start() {
  // TODO(akshay): This function's code is repeated in all ops. Refactor them.
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
    *result_->mutable_error() = move(result_pair.second);
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

void SmbProxy::FinalizeRestoreFilesOp::HandleNextEntity() {
  const int32 index = ++index_;
  if (index >= arg_.entities_vec_size()) {
    return;
  }

  DCHECK(arg_.entities_vec(index).has_restore_file_handle())
      << arg_.entities_vec(index).ShortDebugString();
  const auto& file_handle = arg_.entities_vec(index).restore_file_handle();

  // For some entity it may be possible that we have neither created temporary
  // file nor created actual file. This can happen when we get an error while
  // creating the entity. For those entities, we still get the Finalize call
  // with should_commit set to false. In such case, we do not do anything.
  if (!file_handle.has_tmp_file_path() && !file_handle.has_file_path()) {
    DCHECK(!arg_.entities_vec(index).should_commit())
        << arg_.entities_vec(index).ShortDebugString();
    return;
  }

  // TODO(rupesh) See if we need to abort the task.

  DCHECK(file_handle.has_file_path()) << file_handle.ShortDebugString();

  if (file_handle.has_tmp_file_path()) {
    const auto& entity_md = file_handle.entity_metadata();
    if (arg_.entities_vec(index).should_commit() &&
        IsTargetFileHardlink(entity_md) &&
        !entity_md.has_first_path_to_hardlink()) {
      // If the file is first path to hardlink, then create hardlink to the tmp
      // file.
      string target_tmp_path =
          GetSmbPath(arg_.header().root_dir() + file_handle.tmp_file_path(),
                     &conn_->context);
      // Create hardlink expects the link path as non-dfs path.
      const auto path = GetSmbPath(
          arg_.header().root_dir() + file_handle.file_path(), nullptr);

      auto ret = smbc_wrapper_create_hardlink(
          target_tmp_path.c_str(), path.c_str(), &conn_->context);

      if (ret == -1) {
        string err_msg = "Failed to create hardlink for file " + path +
                         ", target " + target_tmp_path +
                         ". Error msg: " + conn_->context.error_msg;
        auto error =
            FromSmbErrorMessage(conn_->context.nt_status, move(err_msg));
        *result_->mutable_error_vec(index) = move(error);
        return;
      }
    } else {
      // If tmp_file_path is present and should_commit is false, delete the tmp
      // entity. This will be triggered in case of cancels/errors while I/Os
      // were happening on the tmp file. In that case as the data has not been
      // written completely we should go ahead and cleanup the tmp files.
      if (!arg_.entities_vec(index).should_commit()) {
        auto entity_type =
            ConvertEntityMetadataTypeToEntityType(entity_md.type());
        const auto tmp_path =
            GetSmbPath(file_handle.tmp_file_path(), &conn_->context);
        auto op = DeleteEntityOp::CreatePtr(owner_,
                                            arg_.header(),
                                            tmp_path,
                                            entity_type,
                                            false /* exclusive access */,
                                            conn_);
        auto error = op->Execute();
        if (!error.Ok() && error.type() != Error::kNonExistent) {
          LOG(ERROR) << "Failed to delete temporary file: " << tmp_path
                     << " with error: " << error.ToString();
          *result_->mutable_error_vec(index) = move(error);
        }
        return;
      }

      // If tmp_file_path is present and the data has been completely written
      // only then rename the entity.
      RenameEntity(index);
      return;
    }
  }

  // For all other entity set its attributes.
  SetEntityAttribute(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::FinalizeRestoreFilesOp::RenameEntity(int32 index) {
  DCHECK_LT(index, arg_.entities_vec_size()) << arg_.ShortDebugString();

  // TODO(rupesh) Prune the cache.

  // We have to rename the temporary file to actual file. Following cases are
  // possible.
  // 1. Temporary file exist and it is regular file, rename the file.
  // 2. Temporary file does not exist. This may happen if previous incarnation
  //    of restore operation has already renamed the temporary file. Check
  //    the renamed file whether it exist and if it exist it is regular file.
  // 3. If 1 and 2 does not apply fail the rename operation.
  const auto& file_handle = arg_.entities_vec(index).restore_file_handle();

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
  string path = GetSmbPath(arg_.header().root_dir() + file_handle.file_path(),
                           nullptr /* context */);
  string tmp_path = GetSmbPath(
      arg_.header().root_dir() + file_handle.tmp_file_path(), &conn_->context);

  void* mem_ctx =
      smbc_wrapper_talloc_ctx(4 * kNumBytesInMB, "Entity-Metadata");
  if (!mem_ctx) {
    *result_->mutable_error_vec(index) =
        Error(Error::kInvalid,
              "Failed to create memory context for fetching metadata");
    return;
  }

  // Free mem_ctx once this function returns.
  function<void()> release_mem_ctx_fn = [mem_ctx]() mutable {
    smbc_wrapper_free(mem_ctx);
  };
  ScopedRunner scoped_runner(move(release_mem_ctx_fn));

  EntityMetadata metadata;
  auto error =
      GetEntityMetadata(tmp_path, &conn_->context, mem_ctx, &metadata);

  if (error.Ok()) {
    if (metadata.type() == EntityMetadata::kFile) {
      if (IsUptierTask()) {
        EntityMetadata existing_metadata;
        auto error_existing = GetEntityMetadata(path,
                                                &conn_->context,
                                                mem_ctx,
                                                &existing_metadata,
                                                true /* fetch_symlink_tgt */);
        if (!error_existing.Ok()) {
          // Existing file has error or not found.
          *result_->mutable_error_vec(index) = move(error);
          return;
        }
        // Check if existing entity is symlink and we are replacing it with a
        // regular file.
        Error error;
        if (existing_metadata.attributes() && kSmbAttrReparsePoint) {
          // Check if symlink is pointing to cohesity.
          auto desired_symlink_target =
              GetSymlinkTargetPath(file_handle.entity_metadata().path());
          if (desired_symlink_target.compare(
                  existing_metadata.symlink_target()) != 0) {
            error = Error(Error::kInvalid,
                          "Symlink " + path + "->" +
                              existing_metadata.symlink_target() +
                              " is not a targeting to cohesity path " +
                              desired_symlink_target);
          } else {
            auto ret = smbc_wrapper_rename(tmp_path.c_str(),
                                           path.c_str(),
                                           SMBC_ENTITY_TYPE_REGULAR,
                                           true /* replace if exists */,
                                           &conn_->context);
            if (ret == 0) {
              SetEntityAttribute(index);
              return;
            }
            error = FromSmbErrorMessage(
                conn_->context.nt_status,
                "Failed to rename file from " + tmp_path + " to " + path +
                    ", error: " + conn_->context.error_msg);
          }
        } else {
          // The file is not a symlink.
          error = Error(Error::kInvalid, "File " + path + " is not a symlink");
        }

        // Encountered error on symlink checks or rename. Delete tmp file.
        auto error_delete_tmp =
            smbc_wrapper_delete_object(tmp_path.c_str(), &conn_->context);
        // Failed to delete tmp file.
        if (error_delete_tmp != 0) {
          error = FromSmbErrorMessage(
              conn_->context.nt_status,
              "File " + path + " is not a symlink." +
                  "Failed to delete file from " + tmp_path +
                  ", error: " + conn_->context.error_msg);
        }
        *result_->mutable_error_vec(index) = error;
        return;
      } else {
        // Case 1.
        // Temporary file exist and it is regular file, rename it.
        auto ret = smbc_wrapper_rename_object(
            tmp_path.c_str(), path.c_str(), &conn_->context);
        if (ret != 0) {
          *result_->mutable_error_vec(index) = FromSmbErrorMessage(
              conn_->context.nt_status,
              "Failed to rename file from " + tmp_path + " to " + path +
                  ", error: " + conn_->context.error_msg);
          return;
        }
        SetEntityAttribute(index);
      }
    } else {
      // The temporary file is not a regular file.
      *result_->mutable_error_vec(index) =
          Error(Error::kInvalid,
                "Temporary file " + tmp_path + " is not a regular file");
    }
    return;
  }

  // Case 3 above.
  if (error.type() != Error::kNonExistent) {
    // error case.
    *result_->mutable_error_vec(index) = move(error);
    return;
  }

  // Case 2.
  // Temporary file does not exist. Check whether renamed file exist and it is
  // regular file.
  error = GetEntityMetadata(path, &conn_->context, mem_ctx, &metadata);
  if (!error.Ok()) {
    *result_->mutable_error_vec(index) = move(error);
    return;
  }

  if (metadata.type() != EntityMetadata::kFile) {
    *result_->mutable_error_vec(index) =
        Error(Error::kNonExistent,
              "Renamed file " + path + " is not a regular file");
    return;
  }

  // Set the file attributes.
  SetEntityAttribute(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::FinalizeRestoreFilesOp::SetEntityAttribute(int32 index) {
  DCHECK_LT(index, arg_.entities_vec_size()) << arg_.ShortDebugString();

  const auto& file_handle = arg_.entities_vec(index).restore_file_handle();
  string path = GetSmbPath(arg_.header().root_dir() + file_handle.file_path(),
                           &conn_->context);

  smbc_wrapper_entity_metadata smb_metadata = {0};
  FillSmbMetadata(&smb_metadata, file_handle.entity_metadata());

  // Free acls field of smb_metadata once this function returns.
  function<void()> free_smb_metadata_fn = [smb_metadata]() mutable {
    if (smb_metadata.acls != NULL) {
      free(smb_metadata.acls);
    }
  };
  ScopedRunner scoped_runner(move(free_smb_metadata_fn));

  uint32_t sec_info = kOwnerSecurityInformation | kGroupSecurityInformation |
                      kDaclSecurityInformation;
  // Set basic metadata and ACLs of the entity.
  auto ret = smbc_wrapper_set_metadata(path.c_str(),
                                       &conn_->context,
                                       true /* set_file_info */,
                                       true /* set_acl_info */,
                                       sec_info /* additional_info */,
                                       &smb_metadata);
  if (ret != 0) {
    *result_->mutable_error_vec(index) =
        FromSmbErrorMessage(conn_->context.nt_status,
                            "Failed to set metadata for path " + path +
                                ", error msg: " + conn_->context.error_msg);
    return;
  }
}

//-----------------------------------------------------------------------------

bool SmbProxy::FinalizeRestoreFilesOp::IsUptierTask() {
  return arg_.has_nas_uptier_task() && arg_.nas_uptier_task();
}

//-----------------------------------------------------------------------------

string SmbProxy::FinalizeRestoreFilesOp::GetSymlinkTargetPath(
    const string& file_path) {
  CHECK(IsUptierTask());
  string symlink_target = arg_.nas_uptier_symlink_prefix() + file_path;
  return symlink_target;
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
