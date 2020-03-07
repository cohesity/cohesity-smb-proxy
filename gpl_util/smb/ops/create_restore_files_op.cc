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
// Following steps are taken by this op for each entity in the request. For
// each entity this op is allowed to overwrite the existing entity on the
// target if it already exists.
// 1. If user has requested to create an entity for which previous entity
//    already exist it will delete the previous entity. However, if both
//    previous and requested entities are directories, the previous
//    directory will not be deleted.
// 2. Before creating the new entity, it tries to delete the tmp_entity_name
//    and entity_name created by any previous incarnation of restore operation.
//    If newly created entity is directory, it will skip this step.
// 3. Create the newly requested entity with tmp_entity_name ("__ch__"" +
//    task_id_ + "_" + entity_name). In case of directory entity, symlink
//    entity, hard link entity or regular file of 0 bytes tmp_entity_name
//    will be same as entity_name.

#include <boost/filesystem.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <glog/logging.h>
#include <samba/libsmbclient.h>
#include <string>
#include <utility>

#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/smb/ops/create_restore_files_op.h"
#include "gpl_util/smb/ops/delete_entity_op.h"
#include "gpl_util/smb/ops/fetch_file_handle_op.h"
#include "gpl_util/smb/util.h"

using ::boost::shared_mutex;
using ::boost::unique_lock;
using ::cohesity::apache_util::Error;
using ::cohesity::apache_util::smb::EntityMetadata;
using ::cohesity::apache_util::smb::SmbProxyCreateRestoreFilesArg;
using ::cohesity::apache_util::smb::SmbProxyCreateRestoreFilesResult;
using ::std::make_shared;
using ::std::move;
using ::std::pair;
using ::std::string;

namespace cohesity { namespace gpl_util { namespace smb {

namespace {

// Returns true if it is an uptiering job.
bool IsUptierTask(const SmbProxyCreateRestoreFilesArg& arg) {
  return arg.has_nas_uptier_task() && arg.nas_uptier_task();
}
//-----------------------------------------------------------------------------

// Returns true if restore entity needs a tmp file created. In case of volume
// restore, we need tmp file for zero-size hardlinks as well,
// require_tmp_for_empty_hardlink is added to take care of this.
bool NeedTmpFile(const EntityMetadata& restore_entity_md,
                 bool require_tmp_for_empty_hardlink = false) {
  if (restore_entity_md.type() != EntityMetadata::kFile) {
    return false;
  }

  return restore_entity_md.size() > 0 ||
         (IsTargetFileHardlink(restore_entity_md) &&
          require_tmp_for_empty_hardlink);
}

//-----------------------------------------------------------------------------

// Returns the temporary path to the entity given full path. The entity_name in
// full path is changed to "__ch__" + task_id + "_" + entity_name.
string TemporaryEntityPath(string path, string task_key) {
  ::boost::filesystem::path dir(path);
  const string parent = dir.parent_path().string();
  string ch_str = "/__ch__";
  if (!parent.empty() && parent.back() == '/') {
    // Avoid double slashes.
    ch_str = "__ch__";
  }
  const string name = dir.filename().string();
  return parent + ch_str + task_key + "_" + name;
}

//-----------------------------------------------------------------------------

// Returns alternate path for a hardlink's target.
// For example, if
//   entity.first_path_hardlink() = "/home/cohesity/dir1/dir1/foo/orig_link",
//   current_entity.root_dir() = "/home/cohesity/dir1/dir1",
//   current_entity.path() = "/bar/name_of_this_hardlink"
//   alternate_base_dir = "/ALT",
// then this returns /ALT/dir1/foo/orig_link.
//
// TODO(zheng): Add test cases for this function.
string GetAlternateHardLinkPath(const EntityMetadata& entity,
                                const string& alternate_base_dir) {
  DCHECK(!alternate_base_dir.empty());
  DCHECK(!entity.root_dir().empty());
  DCHECK(entity.has_first_path_to_hardlink());

  // Here we have assumption that first_path_to_hardlink is part of same root
  // dir i.e. if hardlinks are present in different root_dirs, then we return
  // empty string.
  if (!IsPathChildOf(entity.first_path_to_hardlink(), entity.root_dir())) {
    return "";
  }

  // Get last component of entity.root_dir().
  ::boost::filesystem::path src_root(entity.root_dir());
  const string root_dir_name = src_root.filename().string();
  DCHECK(!root_dir_name.empty());

  // Get the location where root_dir_name exactly starts.
  auto idx = entity.root_dir().length() - root_dir_name.length();
  DCHECK(idx != string::npos);

  // Append to "/ALT/";
  return (alternate_base_dir + "/" +
          entity.first_path_to_hardlink().substr(idx));
}

//-----------------------------------------------------------------------------

// Construct a path and a tmp path pair for a hardlink entity.
pair<string, string> MakeHardlinkPaths(
    const EntityMetadata& restore_entity_md,
    const string& task_key,
    const SmbProxyCreateRestoreFilesArg& arg) {
  DCHECK(restore_entity_md.has_first_path_to_hardlink());

  string target_path, target_tmp_path;
  if (arg.has_alternate_location_path()) {
    // Assume first_path_to_hardlink is part of same root_dir as
    // restore_entity. If not then we should return error.
    target_path = GetAlternateHardLinkPath(restore_entity_md,
                                           arg.alternate_location_path());
    if (!target_path.empty()) {
      target_tmp_path = TemporaryEntityPath(target_path, task_key);
    }
  } else {
    target_path = restore_entity_md.first_path_to_hardlink();
    target_tmp_path = TemporaryEntityPath(target_path, task_key);
  }

  return make_pair(target_path, target_tmp_path);
}

}  // namespace

//-----------------------------------------------------------------------------

SmbProxy::CreateRestoreFilesOp::Ptr SmbProxy::CreateRestoreFilesOp::CreatePtr(
    SmbProxy* owner,
    const SmbProxyCreateRestoreFilesArg& arg,
    SmbProxyCreateRestoreFilesResult* result) {
  return Ptr(new CreateRestoreFilesOp(owner, arg, result));
}

//-----------------------------------------------------------------------------

SmbProxy::CreateRestoreFilesOp::CreateRestoreFilesOp(
    SmbProxy* owner,
    const SmbProxyCreateRestoreFilesArg& arg,
    SmbProxyCreateRestoreFilesResult* result)
    : owner_(owner), arg_(arg), result_(result) {
  DCHECK(owner_);
  DCHECK(result_);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::Start() {
  auto error = owner_->ValidateSmbProxyBaseArg(arg_.header());
  if (!error.Ok()) {
    *result_->mutable_error() = move(error);
    return;
  }

  // TODO(akshay): Currently we do this op with just 1 SMB connection. Improve
  // this op to have multiple connections and handle entities parallelly.
  auto result_pair = owner_->GetOrCreateSmbConnection(arg_.header());
  conn_ = result_pair.first;
  if (!conn_) {
    DCHECK(!result_pair.second.Ok());
    *result_->mutable_error() = result_pair.second;
    return;
  }

  for (int32 idx = 0; idx < arg_.entity_metadata_vec_size(); ++idx) {
    DCHECK(arg_.entity_metadata_vec(idx).has_restore_entity_md())
        << arg_.entity_metadata_vec(idx).ShortDebugString();

    auto* result = result_->add_entity_result_vec();
    *result->mutable_error() = Error();

    HandleNextEntity();
  }

  owner_->ReleaseSmbConnection(arg_.header(), move(conn_));
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::HandleNextEntity() {
  const int32 index = ++index_;
  if (index >= arg_.entity_metadata_vec_size()) {
    return;
  }

  const auto& task_key = arg_.header().task_key();
  const auto& entity_md = arg_.entity_metadata_vec(index);

  DCHECK(entity_md.has_restore_file_handle()) << entity_md.DebugString();
  const auto& restore_file_handle = entity_md.restore_file_handle();

  DCHECK(restore_file_handle.has_entity_metadata()) << entity_md.DebugString();
  const auto& restore_entity_md = restore_file_handle.entity_metadata();

  // TODO(vjanga): For hardlinks, verify first_path_to_hardlink has same root
  // directory as that of restore_entity_md.

  // If overwrite_existing_tmp is true (or) tmp file doesn't need to be created
  // during volume restore, proceed with HandleEntity.
  if (arg_.overwrite_existing_tmp() ||
      !NeedTmpFile(restore_entity_md,
                   true /* require_tmp_for_empty_hardlink */)) {
    HandleEntity(index);
    return;
  }

  // Below code path is run only for NAS volume restore.
  DCHECK(!arg_.overwrite_existing_tmp()) << arg_.DebugString();

  DCHECK(restore_file_handle.has_tmp_file_path()) << entity_md.DebugString();
  string tmp_path =
      arg_.header().root_dir() + restore_file_handle.tmp_file_path();

  FetchFileHandleOp::Ptr op = FetchFileHandleOp::CreatePtr(
      owner_, conn_, task_key, tmp_path, true /* create_parent_dir */);
  auto result = op->Execute();
  const auto& error = result.first;
  const bool found_cached = result.second;

  bool skip_handle_entity = false;

  if (!error.Ok()) {
    // Found error in creating tmp file, so skipping.
    skip_handle_entity = true;

    auto* result = result_->mutable_entity_result_vec(index);
    *result->mutable_error() = error;
  } else if (found_cached && !IsTargetFileHardlink(restore_entity_md)) {
    // Found tmp file in cache for non-hardlink file, so skipping.
    skip_handle_entity = true;
  }

  if (skip_handle_entity) {
    return;
  }

  HandleEntity(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::HandleEntity(int32 index) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, arg_.entity_metadata_vec_size());

  DCHECK(arg_.entity_metadata_vec(index).has_restore_entity_md())
      << arg_.entity_metadata_vec(index).ShortDebugString();
  const auto& restore_entity_md =
      arg_.entity_metadata_vec(index).restore_entity_md();
  DCHECK(restore_entity_md.has_type()) << restore_entity_md.ShortDebugString();

  // Following operation we need to perform:
  // 0. If not in OverwriteExistingTmp mode and the tmp file has already been
  //    opened and cached - bail out (nothing to do).
  // 1. If existing_entity_md is present we try to delete the existing entity
  //    first (this entity is already present on destination server. It is not
  //    created by previous incarnation of restore operation). But if
  //    existing_entity_md and restore_entity_md both are directory we skip
  //    step 1, 2 and 3.
  // 2. Once the existing_entity is deleted or existing entity does not exist,
  //    we try to delete the tmp entity or any newly created entity created
  //    by previous incarnation of this restore operation.
  // 3. For regular file with non zero size and whose first_path_to_hardlink is
  //    not set, create the temporary entity. For others (directory, symlink,
  //    regular file of zero size and regular file whose first_path_to_hardlink
  //    is set, we directly create the requested entity.

  // In case of uptier task, the symlink (created during downtiering) will be
  // replaced with the file. So, avoid deleting upfront.
  if (IsUptierTask(arg_)) {
    DeleteAndCreateEntity(index);
    return;
  }
  // Case 2.
  // Previous entity with same name does not exist. Try to delete the
  // tmp_entity_name and entity_name if created by previous incarnation of this
  // restore. If newly created entity is directory it will not be deleted by
  // DeleteAndCreateEntity. If previous entity does not exist then
  // 'existing_entity_md' is populated with error kNonExistent.
  if (!arg_.entity_metadata_vec(index).has_existing_entity_md() ||
      (arg_.entity_metadata_vec(index).existing_entity_md().has_error() &&
       arg_.entity_metadata_vec(index).existing_entity_md().error().type() ==
           Error::kNonExistent)) {
    DeleteAndCreateEntity(index);
    return;
  }

  const auto& existing_entity_md =
      arg_.entity_metadata_vec(index).existing_entity_md();

  // If we encountered an error (non-kNonExistent error), like permission
  // denied error when visiting the existing entity, we should also just return
  // an error for this entity in the result. Otherwise we could end up using
  // incorrect metadata of the existing entity (like type not existent, etc).
  if (existing_entity_md.has_error()) {
    DCHECK_NE(existing_entity_md.error().type(), Error::kNonExistent)
        << existing_entity_md.DebugString();
    auto* result_entity = result_->mutable_entity_result_vec(index);
    *result_entity->mutable_error() = existing_entity_md.error();
    return;
  }

  DCHECK(existing_entity_md.has_type())
      << existing_entity_md.ShortDebugString();

  // Case 1. If newly requested entity and existing entity both are directory,
  // directly return.
  if (existing_entity_md.type() == EntityMetadata::kDirectory &&
      existing_entity_md.type() == restore_entity_md.type()) {
    return;
  }

  // Case 1. Otherwise delete the existing entity first.
  DeleteExistingEntity(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::DeleteAndCreateEntity(int32 index) {
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();

  const auto& restore_entity_md =
      arg_.entity_metadata_vec(index).restore_entity_md();
  const auto& type = restore_entity_md.type();

  // If requested entity is directory we do not delete entity created by
  // previous incarnation of restore operation.
  if (type == EntityMetadata::kDirectory) {
    CreateEntity(index);
    return;
  }

  // We create temporary file only if entity is regular file of non zero size
  // and its first_path_to_hardlink is not set.
  if (NeedTmpFile(restore_entity_md) && arg_.overwrite_existing_tmp() &&
      !restore_entity_md.has_first_path_to_hardlink()) {
    DeleteTmpEntity(index);
    return;
  }

  // Try deleting the entity created by previous incarnation of this op.
  DeleteNewEntity(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::DeleteTmpEntity(int32 index) {
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();

  const auto& restore_file_handle =
      arg_.entity_metadata_vec(index).restore_file_handle();
  const auto& restore_entity_md = restore_file_handle.entity_metadata();

  DCHECK_EQ(restore_entity_md.type(), EntityMetadata::kFile)
      << restore_entity_md.ShortDebugString();
  DCHECK(!restore_entity_md.has_first_path_to_hardlink())
      << restore_entity_md.ShortDebugString();
  DCHECK_GT(restore_entity_md.size(), 0) << restore_entity_md.DebugString();
  DCHECK(restore_file_handle.has_tmp_file_path())
      << restore_file_handle.DebugString();

  const string tmp_path =
      GetSmbPath(restore_file_handle.tmp_file_path(), &conn_->context);

  int32 ret = smbc_wrapper_delete_object(tmp_path.c_str(), &conn_->context);

  // We have to also purge the handle for the deleted file from the
  // connection's cache, otherwise later write operations could end up using
  // invalid handles.
  owner_->PurgeFileHandle(arg_.header().task_key(), tmp_path);

  if (ret == -1) {
    auto error =
        FromSmbErrorMessage(conn_->context.nt_status,
                            "Failed to delete tmp entity: " + tmp_path +
                                " , error msg: " + conn_->context.error_msg);
    if (error.type() != Error::kNonExistent) {
      LOG(ERROR) << error.ToString();
      *result_->mutable_entity_result_vec(index)->mutable_error() =
          move(error);
      return;
    }
  }

  // We are done with deleting the temporary file created by previous
  // incarnation of restore operation. Try deleting the entity created
  // by previous incarnation of restore operation.
  DeleteNewEntity(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::DeleteNewEntity(int32 index) {
  // Avoid deleting object if it is an uptier job.
  if (IsUptierTask(arg_)) {
    CreateEntity(index);
    return;
  }
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();

  const auto& restore_file_handle =
      arg_.entity_metadata_vec(index).restore_file_handle();
  const auto& restore_entity_md = restore_file_handle.entity_metadata();
  const string path =
      GetSmbPath(restore_file_handle.file_path(), &conn_->context);

  const auto& type = restore_entity_md.type();
  DCHECK_NE(type, EntityMetadata::kDirectory)
      << restore_entity_md.ShortDebugString();

  // In case of hardlink file, if the hardlink is pointing to tmp file then
  // return.
  if (!arg_.overwrite_existing_tmp() && type == EntityMetadata::kFile &&
      IsTargetFileHardlink(restore_entity_md)) {
    string target_tmp_path = GetSmbPath(
        arg_.header().root_dir() + restore_file_handle.tmp_file_path(),
        &conn_->context);

    const string dfs_path =
        GetSmbPath(arg_.header().root_dir() + restore_file_handle.file_path(),
                   &conn_->context);

    if (IsHardlinksToEachOther(target_tmp_path, dfs_path)) {
      return;
    }
  }

  int32 ret = smbc_wrapper_delete_object(path.c_str(), &conn_->context);

  if (ret == -1) {
    auto error =
        FromSmbErrorMessage(conn_->context.nt_status,
                            "Failed to delete an entity: " + path +
                                " , error msg: " + conn_->context.error_msg);
    if (error.type() != Error::kNonExistent) {
      LOG(ERROR) << error.ToString();
      *result_->mutable_entity_result_vec(index)->mutable_error() =
          move(error);
      return;
    }
  }

  // We are done with deleting existing entity, temporary entity or entity
  // created by previous incarnation of restore operation. Create the requested
  // entity.
  CreateEntity(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::DeleteExistingEntity(int32 index) {
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();
  DCHECK(arg_.entity_metadata_vec(index).has_existing_entity_md())
      << arg_.ShortDebugString();

  const auto& entity_md = arg_.entity_metadata_vec(index);

  DCHECK(entity_md.has_existing_file_handle()) << entity_md.DebugString();
  const auto& existing_file_handle = entity_md.existing_file_handle();

  DCHECK(existing_file_handle.has_file_path()) << entity_md.DebugString();
  DCHECK(existing_file_handle.has_entity_metadata())
      << entity_md.DebugString();

  const auto& existing_entity_md = existing_file_handle.entity_metadata();
  const auto& path =
      GetSmbPath(arg_.header().root_dir() + existing_file_handle.file_path(),
                 &conn_->context);
  auto entity_type =
      ConvertEntityMetadataTypeToEntityType(existing_entity_md.type());

  auto op = DeleteEntityOp::CreatePtr(owner_,
                                      arg_.header(),
                                      path,
                                      entity_type,
                                      false /* exclusive access */,
                                      conn_);

  auto error = op->Execute();
  if (!error.Ok() && error.type() != Error::kNonExistent) {
    LOG(ERROR) << "DeleteEntityOp failed for " << path
               << " failed with error: " << error.ToString();
    *result_->mutable_entity_result_vec(index)->mutable_error() = move(error);
    return;
  }

  // We have deleted the existing entity, try deleting any temporary or new
  // entity created by previous incarnation of restore operation.
  DeleteAndCreateEntity(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::CreateEntity(int32 index) {
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();

  const auto& restore_file_handle =
      arg_.entity_metadata_vec(index).restore_file_handle();
  const auto& type = restore_file_handle.entity_metadata().type();

  // In volume restore mode create parent dir ahead of the entity.
  if (!arg_.overwrite_existing_tmp() || type == EntityMetadata::kDirectory) {
    auto path =
        GetSmbPath(arg_.header().root_dir() + restore_file_handle.file_path(),
                   &conn_->context);

    if (type != EntityMetadata::kDirectory) {
      path = GetParentDir(path);
    }

    // Always create parent dirs in vol restore mode, otherwise check the
    // arg.
    bool create_parent_dirs =
        !arg_.overwrite_existing_tmp() ||
        (type == EntityMetadata::kDirectory && arg_.create_parent_dirs());

    auto error =
        CreateDirectoryHelper(path, &conn_->context, create_parent_dirs);

    if (!error.Ok()) {
      LOG(ERROR) << "CreateDirectoryHelper failed for " << path
                 << " with error: " << error.ToString();
      *result_->mutable_entity_result_vec(index)->mutable_error() = error;
    }

    if (!error.Ok() || type == EntityMetadata::kDirectory) {
      return;
    }
  }

  DoCreateEntity(index);
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::DoCreateEntity(int32 index) {
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();

  const auto& type =
      arg_.entity_metadata_vec(index).restore_entity_md().type();

  if (type == EntityMetadata::kFile) {
    CreateRegularFile(index);
  } else if (type == EntityMetadata::kSymLink) {
    CreateSymlink(index);
  } else {
    // TODO(rupesh) Implement this.
    LOG(FATAL) << "Not supported.";
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::CreateRegularFile(int32 index) {
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();

  const auto& restore_file_handle =
      arg_.entity_metadata_vec(index).restore_file_handle();
  const auto& restore_entity_md = restore_file_handle.entity_metadata();

  // In volume restore, tmp files path is same for all hardlink files pointing
  // to an inode. If the individual files are selected via GUI to restore, we
  // should restore as regular file, even it has more than 1 hardlinks and
  // IsTargetFileHardlink returns false because the path will be empty in case
  // of individual file restore (root dir will be full path to the file).
  if (!arg_.overwrite_existing_tmp() &&
      IsTargetFileHardlink(restore_entity_md)) {
    // If the file is first path to hardlink, then return. Hardlink is created
    // in FinalizeRestoreFilesOp.
    if (restore_entity_md.has_first_path_to_hardlink()) {
      CreateHardlink(index, true /* is_vol_restore */);
    }
    return;
  }

  // If the individual files are selected via GUI to restore and it has more
  // than 1 hardlinks then the first_path_to_hardlink is not set. So hardlink
  // will not be created.
  if (restore_entity_md.has_first_path_to_hardlink()) {
    CreateHardlink(index);
    return;
  }

  // In volume restore mode care of non-zero-sized files has already been
  // taken of.
  if (!arg_.overwrite_existing_tmp() && restore_entity_md.size()) {
    return;
  }

  string path = arg_.header().root_dir();
  if (restore_entity_md.size() == 0) {
    DCHECK(restore_file_handle.has_file_path())
        << restore_file_handle.DebugString();
    path += restore_file_handle.file_path();
  } else {
    DCHECK(restore_file_handle.has_tmp_file_path())
        << restore_file_handle.DebugString();
    path += restore_file_handle.tmp_file_path();
  }
  path = GetSmbPath(path, &conn_->context);

  VLOG(1) << "Creating entity at " << path;
  int32 ret = smbc_wrapper_create_object(
      path.c_str(), &conn_->context, 1 /* is_file */);

  if (ret < 0) {
    string error_msg = "Failed to create the file " + path +
                       ", error msg: " + conn_->context.error_msg;
    auto error = FromSmbErrorMessage(conn_->context.nt_status, error_msg);
    LOG(ERROR) << error.ToString();
    *result_->mutable_entity_result_vec(index)->mutable_error() = move(error);
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::CreateHardlink(int32 index,
                                                    bool is_vol_restore) {
  DCHECK_LT(index, arg_.entity_metadata_vec_size()) << arg_.ShortDebugString();

  const auto& task_key = arg_.header().task_key();
  const auto& restore_file_handle =
      arg_.entity_metadata_vec(index).restore_file_handle();
  const auto& restore_entity_md = restore_file_handle.entity_metadata();

  DCHECK_GT(restore_entity_md.num_hardlinks(), 1) << arg_.DebugString();

  /* path is non-dfs path, because the hardlink op internally use setinfo
   * FILE_INFO/(Level:0x0b) <src FNUM> <destination path> to set the hardlink.
   * SetInfo Request expects destination path to be a non-dfs path.
   */
  const string path =
      GetSmbPath(arg_.header().root_dir() + restore_file_handle.file_path(),
                 nullptr /* context */);
  int32 ret = 0;
  string target_path;
  if (is_vol_restore) {
    string target_tmp_path = GetSmbPath(
        arg_.header().root_dir() + restore_file_handle.tmp_file_path(),
        &conn_->context);

    ret = smbc_wrapper_create_hardlink(
        target_tmp_path.c_str(), path.c_str(), &conn_->context);

    // For error print.
    target_path = target_tmp_path;
  } else {
    DCHECK(restore_entity_md.has_first_path_to_hardlink())
        << restore_entity_md.ShortDebugString();

    const auto target_paths =
        MakeHardlinkPaths(restore_entity_md, task_key, arg_);
    target_path = GetSmbPath(arg_.header().root_dir() + target_paths.first,
                             &conn_->context);
    const auto& target_tmp_path = GetSmbPath(
        arg_.header().root_dir() + target_paths.second, &conn_->context);

    if (target_path.empty()) {
      auto error = Error(Error::kNonExistent,
                         "Failed to create hardlink, target path is empty, " +
                             restore_entity_md.ShortDebugString());
      LOG(ERROR) << error.ToString();
      *result_->mutable_entity_result_vec(index)->mutable_error() =
          move(error);
      return;
    }

    // first_path_to_hardlink (target_path) may be created as temporary entity
    // by restore operation (if target_path is of non-zero size). Once the data
    // for target_path is synced we rename the entity to its original name. At
    // this point we do not know whether the target_path exist or its temporary
    // path exist. We first try to create the hard link to temporary target
    // path, if we failed try to create hard link to target path.
    ret = smbc_wrapper_create_hardlink(
        target_tmp_path.c_str(), path.c_str(), &conn_->context);
    if (ret == -1) {
      auto nt_status = conn_->context.nt_status;
      if (nt_status == kNtStatusObjectNameNotFound ||
          nt_status == kNtStatusObjectPathNotFound) {
        ret = smbc_wrapper_create_hardlink(
            target_path.c_str(), path.c_str(), &conn_->context);
      }
    }
  }

  if (ret == -1) {
    string err_msg = "Failed to create hardlink for file " + path +
                     ", target " + target_path + ". Error msg: " +
                     conn_->context.error_msg;
    auto error = FromSmbErrorMessage(conn_->context.nt_status, move(err_msg));
    // If there is cache miss and a concurrent Create RPC (from a workunit of
    // same hardlink file with a different offset) is processed, we could reach
    // here. So also ignore kAlreadyExists case.
    if (error.type() != Error::kAlreadyExists) {
      LOG(ERROR) << error.ToString();
      *result_->mutable_entity_result_vec(index)->mutable_error() =
          move(error);
    }
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::CreateRestoreFilesOp::CreateSymlink(int32 index) {
  // TODO(akshay): Implement this.
  auto* result = result_->mutable_entity_result_vec(index);
  *result->mutable_error() =
      Error(Error::kInvalid, "Creating a symlink is not implemented.");
  LOG(ERROR) << result->error().error_detail();
}

bool SmbProxy::CreateRestoreFilesOp::IsHardlinksToEachOther(
    const string& target, const string& link) {
  EntityMetadata link_md;
  auto error = GetEntityMetadata(link, &conn_->context, &link_md);
  if (!error.Ok()) {
    return false;
  }

  EntityMetadata target_md;
  error = GetEntityMetadata(target, &conn_->context, &target_md);
  if (!error.Ok() || !IsTargetFileHardlink(target_md)) {
    return false;
  }

  return target_md.inode_id() == link_md.inode_id();
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
