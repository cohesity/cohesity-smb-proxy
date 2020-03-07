// Copyright 2017 Cohesity Inc.
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
// TODO(zheng): Add http traces for this proxy for better debugging.

#include <algorithm>
#include <boost/thread/locks.hpp>
#include <functional>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gperftools/malloc_extension.h>
#include <memory>
#include <simple_web_server/server_http.hpp>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <unordered_set>

#include "apache_util/base/basictypes.h"
#include "apache_util/base/scoped_runner.h"
#include "apache_util/http_server/common_http_server.h"
#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/base/id_locker.h"
#include "gpl_util/base/thread_pool.h"
#include "gpl_util/base/util.h"
#include "gpl_util/smb/ops/create_restore_files_op.h"
#include "gpl_util/smb/ops/create_symlink_op.h"
#include "gpl_util/smb/ops/delete_entity_op.h"
#include "gpl_util/smb/ops/finalize_restore_files_op.h"
#include "gpl_util/smb/ops/rename_entity_op.h"
#include "gpl_util/smb/ops/verify_share_op.h"
#include "gpl_util/smb/ops/write_files_data_op.h"
#include "gpl_util/smb/smb_proxy.h"
#include "gpl_util/smb/util.h"

using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

using boost::shared_lock;
using boost::shared_mutex;
using boost::unique_lock;
using cohesity::apache_util::Error;
using cohesity::apache_util::ScopedRunner;
using cohesity::apache_util::smb::SmbProxyGetStatusInfoArg;
using cohesity::apache_util::smb::SmbProxyGetStatusInfoResult;
using cohesity::apache_util::smb::SmbProxyBaseArg;
using cohesity::apache_util::smb::EntityMetadata;
using cohesity::apache_util::smb::EntityType;
using cohesity::apache_util::smb::SmbProxyGetRootMetadataArg;
using cohesity::apache_util::smb::SmbProxyGetRootMetadataResult;
using cohesity::apache_util::smb::SmbProxyFetchChildrenMetadataArg;
using cohesity::apache_util::smb::SmbProxyFetchChildrenMetadataResult;
using cohesity::apache_util::smb::SmbProxyConnectionCookie;
using cohesity::apache_util::smb::SmbProxyFetchFileDataArg;
using cohesity::apache_util::smb::SmbProxyFetchFileDataResult;
using cohesity::apache_util::smb::SmbProxyWriteFileDataArg;
using cohesity::apache_util::smb::SmbProxyWriteFileDataResult;
using cohesity::apache_util::smb::SmbProxyPurgeTaskArg;
using cohesity::apache_util::smb::SmbProxyPurgeTaskResult;
using cohesity::apache_util::smb::SmbProxyCreateEntityArg;
using cohesity::apache_util::smb::SmbProxyCreateEntityResult;
using cohesity::apache_util::smb::SmbProxyDeleteEntityArg;
using cohesity::apache_util::smb::SmbProxyDeleteEntityResult;
using cohesity::apache_util::smb::SmbProxyCreateRestoreFilesArg;
using cohesity::apache_util::smb::SmbProxyCreateRestoreFilesResult;
using cohesity::apache_util::smb::SmbProxyWriteFilesDataArg;
using cohesity::apache_util::smb::SmbProxyWriteFilesDataResult;
using cohesity::apache_util::smb::SmbProxyFinalizeRestoreFilesArg;
using cohesity::apache_util::smb::SmbProxyFinalizeRestoreFilesResult;
using cohesity::apache_util::smb::SmbProxyCreateSymlinkArg;
using cohesity::apache_util::smb::SmbProxyCreateSymlinkResult;
using cohesity::apache_util::smb::SmbProxyRenameEntityArg;
using cohesity::apache_util::smb::SmbProxyRenameEntityResult;
using cohesity::apache_util::smb::SmbProxyRpcService;
using cohesity::apache_util::smb::SmbProxyVerifyShareArg;
using cohesity::apache_util::smb::SmbProxyVerifyShareResult;
using cohesity::gpl_util::grpclib::RpcContext;
using std::chrono::seconds;
using std::function;
using std::make_shared;
using std::make_unique;
using std::min;
using std::move;
using std::pair;
using std::replace;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::unordered_set;

DEFINE_int32(smb_proxy_alarm_handler_invoke_period_secs, 30,
             "The period in seconds that the SMB proxy's AlarmHandler should "
             "be invoked.");

DEFINE_int32(smb_proxy_task_cache_inactive_secs, 10 * 60 /* 10 mins */,
             "For how many seconds if a task cache has not been used should "
             "consider it as inactive and purge it.");

DEFINE_int32(smb_proxy_connection_cache_inactive_secs, 5 * 60,
             "For how many seconds if a connection cache has not been used "
             "should we consider it as inactive and purge it.");

DEFINE_int32(smb_proxy_create_connection_retry_count, 3,
             "The number of times to retry connection creation request before "
             "failing the request.");

DEFINE_int32(smb_proxy_file_max_read_retries, 2,
             "The maximum number of times to retry reading data from a file "
             "if the read fails.");

DEFINE_int32(smb_proxy_file_read_write_timeout_secs, 60,
             "The timeout in seconds for file read or write operations.");

DEFINE_int32(smb_proxy_file_max_write_retries, 2,
             "The maximum number of times to retry writing data to a file "
             "if the write fails.");

DEFINE_int32(smb_proxy_http_server_port, 20003,
             "HTTP server port for SMB proxy monitoring and stats.");

DEFINE_int32(smb_proxy_task_file_handles_cache_size, 1000,
             "The number of entries allowed to be cached for each task.");

DEFINE_uint64(smb_proxy_max_fh_cache_size, 1000,
              "The max number of allowed entries in the file handles cache "
              "for a task.");

DEFINE_bool(smb_proxy_check_memory_usage, true,
            "Whether to check for memory usage periodically.");

DEFINE_bool(smb_proxy_show_cached_file_handles, true,
            "Whether to show cached file handles of a connection.");

DEFINE_int32(smb_proxy_fetch_children_update_task_threshold,
             16,
             "Number of GetEntityMetada calls after which task access time is "
             "updated.");

//-----------------------------------------------------------------------------

namespace cohesity { namespace gpl_util { namespace smb {

namespace {

// Unpacks the cookie in arg to get the creation_time_usecs and
// children_fetched. If the cookie is not set, a default {-1, 0} pair is
// returned.
pair<int64, int64> UnpackFromCookie(
    const SmbProxyFetchChildrenMetadataArg& arg) {
  if (!arg.has_cookie()) {
    return {-1, 0};
  }

  SmbProxyConnectionCookie cookie;
  CHECK(cookie.ParseFromString(arg.cookie())) << "Invalid cookie received: "
                                              << arg.cookie();

  return {cookie.creation_time_usecs(), cookie.children_fetched()};
}

//-----------------------------------------------------------------------------

// Packs the creation_time_usecs and children_fetched into the cookie in
// result.
void PackToCookie(int64 creation_time_usecs,
                  int64 children_fetched,
                  SmbProxyFetchChildrenMetadataResult* result) {
  SmbProxyConnectionCookie cookie;
  cookie.set_creation_time_usecs(creation_time_usecs);
  cookie.set_children_fetched(children_fetched);
  cookie.SerializeToString(result->mutable_cookie());
}

}  // namespace

struct SmbProxy::State {
  // The SMB proxy RPC service to register to the gRPC server.
  std::unique_ptr<apache_util::smb::SmbProxyRpcService::AsyncService>
      smb_rpc_service_;

  // The port rpc server listen's on.
  int32 rpc_port_;

  // The http port for SMB proxy's http server.
  int32 http_port_;

  // The HTTP server object.
  std::unique_ptr<SimpleWeb::Server<SimpleWeb::HTTP>> http_server_;

  // Indicates whether this service has been stopped or not.
  bool is_stopped_ = false;

  // ID locker to synchronize operations.
  IDLocker<std::string> id_locker_;

  // Mutex for protecting the shared objects below.
  mutable boost::shared_mutex mutex_;

  // The map for task cache. The key is the task key (usually a string
  // containing the task id), and the value is the task cache entry.
  std::unordered_map<std::string, std::shared_ptr<TaskCacheEntry>>
      task_cache_map_;

  // A map from a task key to the FileHandlesEntry for that task.
  std::unordered_map<std::string, std::shared_ptr<FileHandlesEntry>>
      file_handles_map_;

  // A thread pool to run the alarm handler and http server. The proper cleanup
  // of thread pool class depends on order of class member declration and it is
  // better if declared towards the end. Pendantically, it should be declared
  // after all the resources which the threads running in this pool will
  // access.
  ThreadPool thread_pool_{2};
};

//-----------------------------------------------------------------------------

SmbProxy::FileHandlesEntry::FileHandlesEntry()
    : fh_cache(FLAGS_smb_proxy_task_file_handles_cache_size) {}

//-----------------------------------------------------------------------------

SmbProxy::SmbProxy(int32 rpc_port)
    : SmbProxy(rpc_port, FLAGS_smb_proxy_http_server_port) {}

//-----------------------------------------------------------------------------

SmbProxy::SmbProxy(int32 rpc_port, int32 http_port)
    : state_(make_unique<State>()) {
  Init(rpc_port, http_port);

  // Initialize the samba client library.
  smbc_wrapper_initialize();
}

//-----------------------------------------------------------------------------

void SmbProxy::Init(int32 rpc_port, int32 http_port) {
  state_->rpc_port_ = rpc_port;
  state_->http_port_ = http_port;

  state_->smb_rpc_service_ = make_unique<SmbProxyRpcService::AsyncService>();

  CHECK_GE(state_->rpc_port_, 0);
  CHECK(state_->thread_pool_.Init().Ok());
}

//-----------------------------------------------------------------------------

IDLocker<std::string>& SmbProxy::id_locker() { return state_->id_locker_; }

//-----------------------------------------------------------------------------

SmbProxy::~SmbProxy() {
  // Mark the alarm handler to not run for next time.
  state_->is_stopped_ = true;

  // Signal the http server to finish.
  state_->http_server_->stop();

  smbc_wrapper_destroy();

  // ThreadPool destructor will take care of destorying threads once all
  // work units scheduled have finished their work.
}

//-----------------------------------------------------------------------------

void SmbProxy::Start() {
  // Start the gRPC server.
  StartServer(state_->rpc_port_);
  // Start HTTP server.
  state_->http_server_ =
      apache_util::http_server::CreateCommonHttpServer(state_->http_port_);

  RegisterHTTPHandlers();

  state_->thread_pool_.QueueWork([this] { state_->http_server_->start(); });

  // Use a different thread to sleep and periodically run the smb_proxy's
  // AlarmHandler.
  function<void()> alarm_cb = [this] {
    while (!state_->is_stopped_) {
      sleep(FLAGS_smb_proxy_alarm_handler_invoke_period_secs);
      AlarmHandler();
    }
  };

  state_->thread_pool_.QueueWork(move(alarm_cb));
}

//-----------------------------------------------------------------------------

void SmbProxy::AlarmHandler() {
  const auto now_usecs = GetNowInUsecs();

  // Acquire an exclusive lock on task_cache_map_.
  unique_lock<shared_mutex> lock(state_->mutex_);

  // Loop through the tasks and purge any inactive ones.
  for (auto it = state_->task_cache_map_.begin();
       it != state_->task_cache_map_.end();) {
    auto& entry = it->second;

    if (now_usecs - entry->last_access_time_usecs <=
        FLAGS_smb_proxy_task_cache_inactive_secs * kNumUsecsInSec) {
      // Check all inactive connections in this task, and purge any connection
      // that is idle for longer than the threshold, because it will be
      // automatically disconnected by the SMB server in this case. Otherwise
      // if the client tries to use such connection, it will get a
      // NT_STATUS_CONNECTION_DISCONNECTED error.
      MaybePurgeTaskIdleConnections(it->first, it->second);
      LOG(INFO) << "After purging, task " << it->first << " still has "
                << entry->active_connection_map.size()
                << " active connections and "
                << entry->inactive_connection_list.size()
                << " inactive connections";
      ++it;
      continue;
    }

    // If this task has not been active recently, purge all connections. Even
    // if there are still active connections, they are usually left-over by
    // listing directories with large number of children. Since they have not
    // been recently used, they are not actively used now. So it is safe to
    // force purging the task here.
    PurgeTaskAndAllConnections(it->first, it->second, true /* force_purge */);

    // Remove this task from the task cache.
    it = state_->task_cache_map_.erase(it);
  }

  LOG(INFO) << "There are " << state_->task_cache_map_.size()
            << " active tasks";

  // Purge any FileHandlesEntry that was not used in the last hour.
  for (auto it = state_->file_handles_map_.begin();
       it != state_->file_handles_map_.end();) {
    const auto& entry = it->second;
    if (entry->last_access_time_usecs >= (now_usecs - kNumUsecsInHour)) {
      ++it;
      continue;
    }
    it = state_->file_handles_map_.erase(it);
  }

  // Check whether the memory usage of this process has exceeded its limit.
  if (FLAGS_smb_proxy_check_memory_usage) {
    CheckMemoryUsage();
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::RegisterServices(grpc::ServerBuilder* builder) {
  builder->RegisterService(state_->smb_rpc_service_.get());
}

//-----------------------------------------------------------------------------

void SmbProxy::RegisterRpcMethods() {
  REGISTER_GRPC_METHOD(SmbProxyCreateEntity, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyDeleteEntity, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyGetRootMetadata, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyGetStatusInfo, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyFetchChildrenMetadata,
                       state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyFetchFileData, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyPurgeTask, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyWriteFileData, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyCreateRestoreFiles, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyWriteFilesData, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyFinalizeRestoreFiles, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyCreateSymlink, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyRenameEntity, state_->smb_rpc_service_);
  REGISTER_GRPC_METHOD(SmbProxyVerifyShare, state_->smb_rpc_service_);
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyGetStatusInfo(const SmbProxyGetStatusInfoArg& arg,
                                     SmbProxyGetStatusInfoResult* result) {
  // TODO(zheng): Add additional information in the result that might be
  // helpful for debugging.
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyGetRootMetadata(const SmbProxyGetRootMetadataArg& arg,
                                       SmbProxyGetRootMetadataResult* result) {
  // Set the root_dir field if required.
  auto* metadata = result->mutable_metadata();
  if (arg.header().populate_root_dir()) {
    metadata->set_root_dir(arg.header().root_dir());
  }

  auto error = ValidateSmbProxyBaseArg(arg.header());
  if (!error.Ok()) {
    *metadata->mutable_error() = error;
    if (!arg.header().continue_on_error()) {
      *result->mutable_error() = error;
    }
    return;
  }

  auto result_pair = GetOrCreateSmbConnection(arg.header());
  auto conn = result_pair.first;
  if (!conn) {
    CHECK(!result_pair.second.Ok());
    *metadata->mutable_error() = result_pair.second;
    if (!arg.header().continue_on_error()) {
      *result->mutable_error() = move(result_pair.second);
    }
    return;
  }

  void* mem_ctx =
      smbc_wrapper_talloc_ctx(256 * kNumBytesInKB, "Get-Root-Metadata");
  if (!mem_ctx) {
    *metadata->mutable_error() = Error(
        Error::kInvalid,
        "Failed to create memory context when getting root metadata, error "
        "msg: " + string(conn->context.error_msg));
    if (!arg.header().continue_on_error()) {
      *result->mutable_error() = result->metadata().error();
    }
    ReleaseSmbConnection(arg.header(), move(conn));
    return;
  }

  // Cleanup work once this function returns.
  function<void()> cleanup_fn = [this, mem_ctx, &arg, conn]() mutable {
    smbc_wrapper_free(mem_ctx);
    ReleaseSmbConnection(arg.header(), move(conn));
  };
  ScopedRunner scoped_runner(move(cleanup_fn));

  string entity_path = arg.header().root_dir();
  if (!arg.path().empty()) {
    entity_path += arg.path();
  }
  entity_path = GetSmbPath(entity_path, &conn->context);

  if (!arg.path().empty()) {
    metadata->set_path(arg.path());
  }

  // Get the metadata of the root/path.
  error = GetEntityMetadata(entity_path, &conn->context, mem_ctx, metadata);
  if (!error.Ok()) {
    *metadata->mutable_error() = error;
    if (!arg.header().continue_on_error()) {
      *result->mutable_error() = move(error);
    }
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyFetchChildrenMetadata(
    const SmbProxyFetchChildrenMetadataArg& arg,
    SmbProxyFetchChildrenMetadataResult* result) {
  auto error = ValidateSmbProxyBaseArg(arg.header());
  if (!error.Ok()) {
    if (!arg.header().continue_on_error()) {
      result->mutable_error()->CopyFrom(error);
    }
    *result->mutable_parent_dir_error() = move(error);
    return;
  }

  const auto& task_key = arg.header().task_key();
  string share_path = arg.header().server() + arg.header().share();
  auto cookie = UnpackFromCookie(arg);
  auto result_pair = GetOrCreateSmbConnection(
      arg.header(), "" /* handle_path */, cookie.first, cookie.second);
  auto conn = result_pair.first;
  if (!conn) {
    CHECK(!result_pair.second.Ok());
    if (result_pair.second.type() != Error::kRetry) {
      // We should not set the parent_dir_error if it is kRetry, because the
      // client will retry from the beginning.
      *result->mutable_parent_dir_error() = result_pair.second;
    }
    if (!arg.header().continue_on_error() ||
        result_pair.second.type() == Error::kRetry) {
      // Always populate the result->error if the specified active connection
      // cannot be found, so that the client can retry from the beginning.
      *result->mutable_error() = move(result_pair.second);
    }
    return;
  }

  // Create a list dir expression. We need to replace all / in the path with \,
  // and add a * at the end in order to work with samba client's list dir
  // function.
  string list_exp = arg.header().root_dir() + arg.path() + "\\*";
  list_exp = GetSmbPath(list_exp, &conn->context);

  uint32 size = 0;
  uint16 fnum = conn->dir_handle;

  void* mem_ctx =
      smbc_wrapper_talloc_ctx(4 * kNumBytesInMB, "Fetch-Children-Metadata");
  if (!mem_ctx) {
    *result->mutable_parent_dir_error() =
        Error(Error::kInvalid,
              "Failed to create memory context when fetching children for " +
              arg.path() + ", error msg: " + conn->context.error_msg);
    if (!arg.header().continue_on_error()) {
      *result->mutable_error() = result->parent_dir_error();
    }
    ReleaseSmbConnection(arg.header(), move(conn));
    return;
  }

  // Free mem_ctx once this function returns below this point.
  function<void()> release_mem_ctx_fn = [mem_ctx]() mutable {
    smbc_wrapper_free(mem_ctx);
  };
  ScopedRunner scoped_runner(move(release_mem_ctx_fn));

  smbc_wrapper_readirplus_entry** readirplus_entries = nullptr;
  char **names = nullptr;
  if (arg.fetch_acls()) {
    names = smbc_wrapper_list_dir(
        list_exp.c_str(), &conn->context, mem_ctx, &fnum, &size);
  } else {
    readirplus_entries = smbc_wrapper_list_dirplus(
        list_exp.c_str(), &conn->context, mem_ctx, &fnum, &size);
  }

  if (!readirplus_entries && !names) {
    auto error =
        FromSmbErrorMessage(conn->context.nt_status,
                            "Failed to list children names for " + list_exp +
                                ", error msg: " + conn->context.error_msg);
    if (IsConnectionUnusable(conn->context.nt_status)) {
      // Returns a kRetry error, so that the caller will retry from beginning
      // for this directory.
      DCHECK_EQ(error.type(), Error::kRetry);
      *result->mutable_error() = move(error);
      // Shut down this already disconnected connection and remove it from this
      // task.
      ShutdownSmbConnection(task_key, share_path, conn);
      ReleaseSmbConnection(arg.header(), move(conn), false /* reuse */);
      return;
    }

    *result->mutable_parent_dir_error() = move(error);
    if (!arg.header().continue_on_error()) {
      *result->mutable_error() = result->parent_dir_error();
    }
    ReleaseSmbConnection(arg.header(), move(conn));
    return;
  }

  // Build the hash set for children which we need to skip fetching metadata.
  unordered_set<string> skip_metadata_set;
  for (auto path_to_skip : arg.header().skip_metadata_vec()) {
    skip_metadata_set.emplace(move(path_to_skip));
  }

  // Get the metadata (including ACLs) for all the children.
  // TODO(zheng): Use more threads to improve the performance.
  for (uint32 ii = 0; ii < size; ++ii) {
    if (ii % FLAGS_smb_proxy_fetch_children_update_task_threshold == 0) {
      UpdateTaskAccessTime(task_key);
    }

    if (readirplus_entries) {
      smbc_wrapper_readirplus_entry* src = readirplus_entries[ii];

      string path = arg.path() + string("/") + string(src->name);
      if (skip_metadata_set.count(path) > 0) {
        continue;
      }

      auto* child_md = result->add_children_metadata();
      child_md->set_uid(0);
      child_md->set_gid(0);
      child_md->set_attributes(src->metadata.attributes);
      child_md->set_change_time_usecs(
          ConvertTimespecToUsecs(&src->metadata.change_time_ts));
      child_md->set_modify_time_usecs(
          ConvertTimespecToUsecs(&src->metadata.modify_time_ts));
      child_md->set_create_time_usecs(
          ConvertTimespecToUsecs(&src->metadata.create_time_ts));
      child_md->set_access_time_usecs(
          ConvertTimespecToUsecs(&src->metadata.access_time_ts));
      child_md->set_size(src->metadata.size);
      child_md->set_allocation_size(src->metadata.allocation_size);
      child_md->set_inode_id(src->metadata.inode_id);
      child_md->set_path(move(path));
      if (arg.header().populate_root_dir()) {
        child_md->set_root_dir(arg.header().root_dir());
      }

      if (child_md->attributes() & kSmbAttrDirectory) {
        child_md->set_type(EntityMetadata::kDirectory);
      } else {
        child_md->set_type(EntityMetadata::kFile);
      }
      continue;
    }

    auto* name = names[ii];
    string path = arg.path() + string("/") + string(name);
    if (skip_metadata_set.count(path) > 0) {
      continue;
    }

    auto* child_md = result->add_children_metadata();
    if (arg.header().populate_root_dir()) {
      child_md->set_root_dir(arg.header().root_dir());
    }
    child_md->set_path(path);

    if (!arg.header().root_dir().empty()) {
      path = arg.header().root_dir() + path;
    }
    path = GetSmbPath(path, &conn->context);
    auto error = GetEntityMetadata(
        path, &conn->context, mem_ctx, child_md, arg.fetch_symlink_target());
    if (!error.Ok()) {
      *child_md->mutable_error() = error;
      if (arg.header().continue_on_error()) {
        continue;
      }

      *result->mutable_error() = move(error);
      break;
    }
  }

  if (names) {
    // Free memory allocated in libsmbclient proactively.
    smbc_wrapper_free_names(names, size);
  }

  // If we cannot continue due to a failure, we might need to shutdown the
  // connection.
  if (result->has_error()) {
    if (fnum == SMBC_WRAPPER_INVALID_FNUM) {
      // If fnum is SMBC_WRAPPER_INVALID_FNUM, it means that it has already
      // been closed by smbc_wrapper_list_dir, so we don't need to close it.
      ReleaseSmbConnection(arg.header(), move(conn));
    } else {
      // Otherwise, just call ShutdownSmbConnection to both close the dir
      // handle and shutdown the connection since the client will stop after
      // seeing the failure.
      conn->dir_handle = fnum;
      LOG(INFO) << "Force shutting down connection to " << share_path
                << " because of a failure";
      ShutdownSmbConnection(task_key, share_path, conn);

      // Also remove the connection from the active connection map.
      ReleaseSmbConnection(arg.header(), conn, false /* reuse */);
    }

    return;
  }

  // If we have reached the end of this directory (indicated by a
  // SMBC_WRAPPER_INVALID_FNUM value of fnum), we should just release the
  // connection since the last call to smbc_wrapper_list_dir already closed the
  // directory handle. If we let the alarm handler to purge instead, it will
  // need to close it because it will be treated as left over handle from a
  // previously crashed client.
  if (fnum == SMBC_WRAPPER_INVALID_FNUM) {
    ReleaseSmbConnection(arg.header(), move(conn));
  } else {
    conn->dir_handle = fnum;
    conn->children_fetched += size;
    PackToCookie(conn->creation_time_usecs, conn->children_fetched, result);
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyFetchFileData(const SmbProxyFetchFileDataArg& arg,
                                     SmbProxyFetchFileDataResult* result) {
  const auto& header = arg.header();
  auto error = ValidateSmbProxyBaseArg(header);
  if (!error.Ok()) {
    *result->mutable_error() = move(error);
    return;
  }

  // Check whether the requested size falls in supported range.
  if (arg.size() <= 0) {
    *result->mutable_error() =
        Error(Error::kInvalid,
              "Size must be greater than 0 for SmbProxyFetchFileData");
    return;
  }

  string path = header.root_dir() + arg.file_path();

  auto result_pair =
      GetOrCreateSmbConnection(header, arg.cache_handle() ? path : "");
  auto conn = result_pair.first;
  if (!conn) {
    CHECK(!result_pair.second.Ok());
    *result->mutable_error() = move(result_pair.second);
    return;
  }

  // Samba uses uint32 as size type.
  const uint32 requested_size = static_cast<uint32>(arg.size());
  const uint32 max_read_size = conn->context.max_read_size;
  DCHECK_GT(max_read_size, 0);

  uint32 bytes_to_read = requested_size;
  uint32 bytes_read = 0;
  // Use a string buffer allocated on the heap, which will be owned and
  // deallocated by the result proto message when we can return the buffer as
  // result, otherwise it will be deallocated by this function.
  string* buf_str = new string();
  buf_str->reserve(requested_size);
  while (bytes_to_read > 0) {
    uint32 read_size = min(bytes_to_read, max_read_size);
    auto ret =
        ReadBytesFromOffset(path,
                            conn,
                            arg.offset() + bytes_read,
                            read_size,
                            buf_str,
                            bytes_read /* buf_offset */,
                            arg.cache_handle());

    bytes_to_read -= ret.first;
    bytes_read += ret.first;
    CHECK_GE(requested_size, bytes_to_read);
    CHECK_GE(requested_size, bytes_read);

    error = move(ret.second);
    if (!error.Ok()) {
      if (bytes_read > 0) {
        // The call has read some partial bytes, so return whatever what we
        // have read here.
        buf_str->shrink_to_fit();
        result->set_allocated_data(buf_str);
      } else {
        delete buf_str;
      }

      // If the error is kRetry, it means that it is a disconnected connection,
      // in which case we need to shut down the connection purge it from the
      // cache.
      if (error.type() == Error::kRetry) {
        ShutdownSmbConnection(
            header.task_key(), header.server() + header.share(), conn);
        ReleaseSmbConnection(header, move(conn), false /* reuse */);
      } else {
        ReleaseSmbConnection(header, move(conn));
      }

      *result->mutable_error() = move(error);
      return;
    }

    // The call to ReadBytesFromOffset should have read all 'read_size' of
    // bytes here.
    DCHECK_EQ(read_size, ret.first);
  }

  CHECK_EQ(bytes_read, requested_size);
  result->set_allocated_data(buf_str);
  ReleaseSmbConnection(header, move(conn));
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyWriteFileData(const SmbProxyWriteFileDataArg& arg,
                                     SmbProxyWriteFileDataResult* result) {
  const auto& header = arg.header();
  auto error = ValidateSmbProxyBaseArg(header);
  if (!error.Ok()) {
    *result->mutable_error() = move(error);
    return;
  }

  // Check whether the requested size falls in supported range.
  if (arg.size() <= 0) {
    *result->mutable_error() =
        Error(Error::kInvalid,
              "Size must be greater than 0 for SmbProxyWriteFileData");
    return;
  }

  if (arg.data().empty()) {
    *result->mutable_error() = Error(Error::kInvalid, "data cannot be empty");
    return;
  }

  string path = header.root_dir() + arg.file_path();

  auto result_pair =
      GetOrCreateSmbConnection(header, arg.cache_handle() ? path : "");
  auto conn = result_pair.first;
  if (!conn) {
    CHECK(!result_pair.second.Ok());
    *result->mutable_error() = move(result_pair.second);
    return;
  }

  error = WriteFileDataHelper(path,
                              conn,
                              arg.data(),
                              arg.size(),
                              arg.offset(),
                              arg.cache_handle(),
                              arg.is_stubbed());

  if (!error.Ok()) {
    // If the error is kRetry, it means that it is a disconnected connection,
    // in which case we need to shut down the connection purge it from the
    // cache.
    if (error.type() == Error::kRetry ||
        error.type() == Error::kPermissionDenied) {
      ShutdownSmbConnection(
          header.task_key(), header.server() + header.share(), conn);
      ReleaseSmbConnection(header, move(conn), false /* reuse */);
    } else {
      ReleaseSmbConnection(header, move(conn));
    }
    *result->mutable_error() = move(error);
    return;
  }

  ReleaseSmbConnection(header, move(conn));
}

//-----------------------------------------------------------------------------

Error SmbProxy::WriteFileDataHelper(const string& path,
                                    SmbConnection::Ptr conn,
                                    const string& data,
                                    int64 size,
                                    int64 offset,
                                    bool cache_handle,
                                    bool is_stubbed) {
  // For a stubbed file remove the trailing zeros. Please see design doc for
  // more details: https://docs.google.com/document/d/
  // 1d-KbgdI5hb2NxbiIyb2okh3g1bV93h9_TDLLIf6zdTo/edit?usp=sharing
  if (is_stubbed) {
    int ignored_bytes = 0;
    while (--size > 0 && !data[size]) {
      ++ignored_bytes;
    }
    ++size;

    LOG(INFO)
        << "Ignored trailing bytes with zero data('\\0') for stubbed file: "
        << ignored_bytes;
  }

  // Samba uses uint32 as size type.
  const uint32 requested_size = static_cast<uint32>(size);
  const uint32 max_write_size = conn->context.max_write_size;
  DCHECK_GT(max_write_size, 0);

  uint32 bytes_to_write = requested_size;
  uint32 bytes_written = 0;
  while (bytes_to_write > 0) {
    uint32 write_size = min(bytes_to_write, max_write_size);
    auto ret = WriteBytesToOffset(path,
                                  conn,
                                  offset + bytes_written,
                                  write_size,
                                  data.c_str() + bytes_written,
                                  cache_handle);

    bytes_to_write -= ret.first;
    bytes_written += ret.first;
    CHECK_GE(requested_size, bytes_to_write);
    CHECK_GE(requested_size, bytes_written);

    auto error = move(ret.second);
    if (!error.Ok()) {
      return error;
    }
    // The call to WriteBytesToOffset should have written all 'write_size' of
    // bytes.
    DCHECK_EQ(write_size, ret.first);
  }

  CHECK_EQ(bytes_written, requested_size);
  return Error();
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyPurgeTask(const SmbProxyPurgeTaskArg& arg,
                                 SmbProxyPurgeTaskResult* result) {
  if (arg.task_key().empty()) {
    *result->mutable_error() =
        Error(Error::kInvalid, "The 'task_key' field cannot be empty");
    return;
  }

  // Acquire an exclusive lock on task_cache_map_.
  unique_lock<shared_mutex> lock(state_->mutex_);
  auto it = state_->task_cache_map_.find(arg.task_key());
  if (it == state_->task_cache_map_.end()) {
    *result->mutable_error() = Error(
        Error::kNonExistent, "The task " + arg.task_key() + " does not exist");
    return;
  }

  auto task_entry = it->second;

  // Remove this task from the task cache and release mutex_.
  state_->task_cache_map_.erase(it);
  lock.unlock();

  // Purge the task and all its connections.
  auto error = PurgeTaskAndAllConnections(arg.task_key(), move(task_entry));

  if (!error.Ok()) {
    *result->mutable_error() = move(error);
  }
}

//-----------------------------------------------------------------------------

Error SmbProxy::ValidateSmbProxyBaseArg(const SmbProxyBaseArg& arg) const {
  if (arg.task_key().empty()) {
    return Error(Error::kInvalid, "The 'task_key' field cannot be empty");
  }
  if (arg.server().empty()) {
    return Error(Error::kInvalid, "The 'server' field cannot be empty");
  }
  if (arg.share().empty()) {
    return Error(Error::kInvalid, "The 'share' field cannot be empty");
  }
  if (arg.share()[0] != '/') {
    return Error(Error::kInvalid, "The 'share' field must start with /");
  }
  if (arg.username().empty()) {
    return Error(Error::kInvalid, "The 'username' field cannot be empty");
  }
  if (arg.password().empty()) {
    return Error(Error::kInvalid, "The 'password' field cannot be empty");
  }

  return Error();
}

//-----------------------------------------------------------------------------

void SmbProxy::UpdateTaskAccessTime(const string& task_key) {
  unique_lock<shared_mutex> lock(state_->mutex_);
  auto iter = state_->task_cache_map_.find(task_key);

  if (iter == state_->task_cache_map_.end()) return;

  iter->second->last_access_time_usecs = GetNowInUsecs();
}

//-----------------------------------------------------------------------------

pair<SmbProxy::SmbConnection::Ptr, Error> SmbProxy::GetOrCreateSmbConnection(
    const SmbProxyBaseArg& arg,
    const string handle_path,
    int64 creation_time_usecs,
    int64 children_fetched) {
  DCHECK(!arg.task_key().empty());

  string share_path = arg.server() + arg.share();
  TaskCacheEntry::Ptr task_entry;

  // Acquire an exclusive lock on task_cache_map_. Find the cache entry for
  // the task, or create one if it does not exist.
  unique_lock<shared_mutex> lock(state_->mutex_);
  auto iter = state_->task_cache_map_.find(arg.task_key());
  if (iter == state_->task_cache_map_.end()) {
    task_entry = make_shared<TaskCacheEntry>();
    task_entry->share_path = share_path;
    state_->task_cache_map_.emplace(arg.task_key(), task_entry);
  } else {
    DCHECK_EQ(share_path, iter->second->share_path);
    task_entry = iter->second;
  }

  DCHECK(task_entry);

  // Update the task entry's access time.
  task_entry->last_access_time_usecs = GetNowInUsecs();

  // Now we can release the lock on task_cache_map_, since we just updated the
  // task's last access time, so that AlarmHandler won't purge this task.
  lock.unlock();

  // Acquire an exclusive lock for this task only.
  unique_lock<shared_mutex> task_lock(task_entry->mutex);

  if (creation_time_usecs != -1) {
    DCHECK(handle_path.empty());

    // We need to find the connection from the active_connection_map since this
    // RPC call specifies the creation_time_usecs as the cookie.
    auto conn_iter =
        task_entry->active_connection_map.find(creation_time_usecs);
    if (conn_iter == task_entry->active_connection_map.end()) {
      // Cannot find this connection in the active map.
      return {nullptr, Error(Error::kRetry, "Cannot find connection")};
    }

    // The directory handle should be a valid one.
    DCHECK_NE(conn_iter->second->dir_handle, SMBC_WRAPPER_INVALID_FNUM);

    // If the children fetched counter do not match, we also return a kRetry
    // error so that the client will retry from the beginning. The connection
    // needs to be shut down in this case.
    if (conn_iter->second->children_fetched != children_fetched) {
      ShutdownSmbConnection(arg.task_key(), share_path, conn_iter->second);
      task_entry->active_connection_map.erase(conn_iter);

      return {nullptr,
              Error(Error::kRetry, "Children fetched counter do not match")};
    }

    // Update the access time.
    conn_iter->second->last_access_time_usecs = GetNowInUsecs();
    return {conn_iter->second, Error()};
  }

  // Try to find and reuse an inactive connection.
  if (!task_entry->inactive_connection_list.empty()) {
    if (!handle_path.empty()) {
      DCHECK_EQ(creation_time_usecs, -1);

      // Try to find the inactive connection which has the cached handle for
      // the request path.
      for (auto it = task_entry->inactive_connection_list.begin();
           it != task_entry->inactive_connection_list.end();
           ++it) {
        auto conn = *it;
        DCHECK(conn);

        // Acquire a shared lock to access LRU map of connection.
        shared_lock<shared_mutex> lock(conn->mutex);
        if (conn->fh_lru_map.count(handle_path) > 0) {
          task_entry->inactive_connection_list.erase(it);
          // Move it to the active map.
          task_entry->active_connection_map.emplace(conn->creation_time_usecs,
                                                    conn);
          // Update the access time.
          conn->last_access_time_usecs = GetNowInUsecs();
          return {move(conn), Error()};
        }
      }
    }

    // Just return the first inactive connection.
    auto conn = task_entry->inactive_connection_list.front();
    task_entry->inactive_connection_list.pop_front();
    DCHECK(conn);
    // Move it to the active map.
    task_entry->active_connection_map.emplace(conn->creation_time_usecs, conn);
    // Update the access time.
    conn->last_access_time_usecs = GetNowInUsecs();
    return {move(conn), Error()};
  }

  // Create a new connection.
  auto conn = make_shared<SmbConnection>();

  // Retry connection creation if it fails.
  int32 ret = -1;
  int32 retry_count = FLAGS_smb_proxy_create_connection_retry_count;
  bool isIpAddress = IsIpAddress(arg.server().c_str());

  while (ret == -1 && retry_count > 0) {
    if (!isIpAddress) {
      ret = CreateSmbConnectionByResolvingHostname(
          arg.server().c_str(),
          // smbc_wrapper_create_connection does not like the starting / or \\.
          arg.share().substr(1).c_str(),
          arg.username().c_str(),
          arg.password().c_str(),
          arg.domain_name().c_str(),
          &conn->context);
    }
    if (ret == -1) {
      ret = smbc_wrapper_create_connection(
          arg.server().c_str(),
          // smbc_wrapper_create_connection does not like the starting / or \\.
          arg.share().substr(1).c_str(),
          arg.username().c_str(),
          arg.password().c_str(),
          arg.domain_name().c_str(),
          &conn->context);
    }
    --retry_count;
  }
  if (ret == -1) {
    // Failed to create a new connection
    return {nullptr,
            FromSmbErrorMessage(
                conn->context.nt_status,
                "Failed to create a new connection to " + share_path +
                    ", error msg: " + string(conn->context.error_msg))};
  }

  // Set the creation and access time.
  conn->creation_time_usecs = GetNowInUsecs();
  conn->last_access_time_usecs = conn->creation_time_usecs;
  task_entry->active_connection_map.emplace(conn->creation_time_usecs, conn);
  LOG(INFO) << "Created a new connection at " << conn->creation_time_usecs
            << " to \\\\" << arg.server() << arg.share()
            << ", with max read size " << conn->context.max_read_size
            << ", with max write size " << conn->context.max_write_size;
  return {move(conn), Error()};
}

//-----------------------------------------------------------------------------

void SmbProxy::ReleaseSmbConnection(const SmbProxyBaseArg& arg,
                                    SmbConnection::Ptr conn,
                                    bool reuse) {
  conn->dir_handle = SMBC_WRAPPER_INVALID_FNUM;
  conn->children_fetched = 0;
  const auto& task_key = arg.task_key();

  // Acquire a shared lock on task_cache_map_.
  shared_lock<shared_mutex> lock(state_->mutex_);
  auto iter = state_->task_cache_map_.find(task_key);
  TaskCacheEntry::Ptr task_entry;

  if (iter == state_->task_cache_map_.end()) {
    LOG(ERROR) << "Failed to find the cache entry for task " << task_key;
    return;
  } else {
    task_entry = iter->second;
  }
  // Release the lock on task_cache_map_.
  lock.unlock();

  // Acquire an exclusive lock for this task only.
  unique_lock<shared_mutex> task_lock(task_entry->mutex);
  auto conn_iter =
      task_entry->active_connection_map.find(conn->creation_time_usecs);
  if (conn_iter == task_entry->active_connection_map.end()) {
    DLOG(FATAL)
        << "Failed to find the connection in active_connection_map for task "
        << task_key;
  } else {
    task_entry->active_connection_map.erase(conn_iter);
  }

  if (reuse) {
    conn->last_access_time_usecs = GetNowInUsecs();
    task_entry->inactive_connection_list.emplace_back(move(conn));
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::ShutdownSmbConnection(const string& task_key,
                                     const string& share_path,
                                     SmbConnection::Ptr conn) {
  DCHECK(conn);

  // First we need to close the directory handle if it is a valid handle.
  if (conn->dir_handle != SMBC_WRAPPER_INVALID_FNUM) {
    LOG(INFO) << "Closing directory handle [" << conn->dir_handle
              << "] for task [" << task_key << "], SMB share [" << share_path
              << "], connection created at [" << conn->creation_time_usecs
              << "]";
    auto ret = smbc_wrapper_close_fnum(&conn->context, conn->dir_handle);
    if (ret == -1) {
      LOG(ERROR) << "Failed to close the directory handle " << conn->dir_handle
                 << " for task [" << task_key << "], SMB share [" << share_path
                 << "], connection created at [" << conn->creation_time_usecs
                 << "], reason: " << conn->context.error_msg;
    }
  }

  // Acquire an exclusive lock to modify LRU cache of connection.
  unique_lock<shared_mutex> lock(conn->mutex);

  // Next we need to close all the cached file handles.
  for (const auto& fh : conn->fh_lru_list) {
    DCHECK_NE(fh.fnum, SMBC_WRAPPER_INVALID_FNUM);

    auto ret = smbc_wrapper_close_fnum(&conn->context, fh.fnum);
    if (ret == -1) {
      LOG(ERROR) << "Failed to close the file handle " << fh.fnum
                 << " for file [" << fh.path << "], task [" << task_key
                 << "], SMB share [" << share_path
                 << "], connection created at [" << conn->creation_time_usecs
                 << "], reason: " << conn->context.error_msg;
    }
  }
  conn->fh_lru_list.clear();
  conn->fh_lru_map.clear();

  // Release the lock.
  lock.unlock();

  // Now shut down the connection.
  LOG(INFO) << "Shutting down connection created at "
            << conn->creation_time_usecs << " to [" << share_path
            << "] for task [" << task_key << "]";
  smbc_wrapper_shutdown_connection(&conn->context);
}

//-----------------------------------------------------------------------------

pair<uint32, Error> SmbProxy::ReadBytesFromOffset(const std::string& path,
                                                  SmbConnection::Ptr conn,
                                                  uint64 offset,
                                                  uint32 size,
                                                  string* buf,
                                                  uint32 buf_offset,
                                                  bool cache_handle) {
  DCHECK(conn);
  DCHECK(buf);
  DCHECK_LT(buf_offset, buf->capacity());

  int32 num_retries = 0;
  uint32 actual_read_size;
  Error error;
  string smb_path = GetSmbPath(path, &conn->context);

  // Passing a nullptr as 'fnum_ptr' to smbc_wrapper_fetch_file_data will
  // open and close the file handle immediately. Passing in a non-nullptr
  // pointer with a SMBC_WRAPPER_INVALID_FNUM value will open a new handle
  // (since the cache cannot be found) without closing it. Passing a
  // non-nullptr with non- SMBC_WRAPPER_INVALID_FNUM value will reuse the
  // handle without closing it.
  uint16 fnum = SMBC_WRAPPER_INVALID_FNUM;
  uint16* fnum_ptr = nullptr;
  if (cache_handle) {
    fnum_ptr = &fnum;

    // Acquire a shared lock to access LRU cache of connection.
    shared_lock<shared_mutex> lock(conn->mutex);
    // Search in the cache to see whether the file has already been opened.
    auto it = conn->fh_lru_map.find(path);
    if (it != conn->fh_lru_map.end()) {
      DCHECK_NE(it->second->fnum, SMBC_WRAPPER_INVALID_FNUM);
      *fnum_ptr = it->second->fnum;
    }
  }

  while (true) {
    // TODO(Zheng): Right now we are doing immediate retries, because we
    // don't have ClosureRunner and done_cb framework for the GRPC service
    // code. Add a delay for this retry so that it has a better chance to
    // succeed.

    actual_read_size = 0;

    // Since we also need to use this memory context to allocate a few other
    // things, so make the capacity 1MB larger than the read size.
    void* mem_ctx =
        smbc_wrapper_talloc_ctx(kNumBytesInMB + size, "Fetch-File-Data");
    if (!mem_ctx) {
      return {0,
              Error(Error::kInvalid,
                    "Failed to create memory context when fetching file "
                    "data, error msg: " +
                        string(conn->context.error_msg))};
    }

    // Free the memory context once out of this scope. Notice that we can
    // only free the memory context instead of the 'data' allocated in this
    // context for each try. Otherwise we will get segfault when we try to
    // free 'data'. Also update the file handle cache if necessary.
    function<void()> cleanup_fn = [this, mem_ctx]() mutable {
      smbc_wrapper_free(mem_ctx);
    };
    ScopedRunner scoped_runner(move(cleanup_fn));

    DCHECK_GT(FLAGS_smb_proxy_file_read_write_timeout_secs, 0);
    uint8* data = smbc_wrapper_fetch_file_data(
        smb_path.c_str(),
        &conn->context,
        mem_ctx,
        offset,
        size,
        &actual_read_size,
        fnum_ptr,
        FLAGS_smb_proxy_file_read_write_timeout_secs * kNumMsecsInSec);
    if (!data) {
      error =
          FromSmbErrorMessage(conn->context.nt_status,
                              "Failed to fetch data for file " + path +
                                  ", error msg: " + conn->context.error_msg);
      LOG(ERROR) << error.error_detail();
      if (IsConnectionUnusable(conn->context.nt_status)) {
        // If the error is because of the connection has been disconnected,
        // directly return the error so that the caller can retry.
        DCHECK_EQ(error.type(), Error::kRetry);
        return {0, error};
      }

      // We don't retry for non-existent file or access denied file, and we
      // don't update the file handle in such cases.
      if (error.type() == Error::kNonExistent ||
          error.type() == Error::kPermissionDenied) {
        return {0, error};
      }
    } else {
      // Copy whatever we have read to the string buffer. Notice that this is
      // the only way we can do this, because std::string does not provide a
      // mutable char* to its buffer, and does not even always allocate the
      // buffer contiguously, so we cannot use &(*buf)[0] as the buffer.
      buf->replace(
          buf_offset, actual_read_size, (const char*)data, actual_read_size);
      if (size == actual_read_size) {
        // We have a successful read.
        error = Error();
        break;
      }

      // We have read fewer bytes than expected, set an error and keep
      // retrying.
      error =
          Error(Error::kInvalid,
                "Read size differs for file " + path + ", requested size [" +
                    to_string(size) + "], actual read size [" +
                    to_string(actual_read_size) + "]");
    }

    DCHECK(!error.Ok());
    ++num_retries;
    if (num_retries <= FLAGS_smb_proxy_file_max_read_retries) {
      LOG(ERROR) << "Retrying fetching file data because of error: "
                 << error.DebugString();
    } else {
      break;
    }
  }

  if (cache_handle && *fnum_ptr != SMBC_WRAPPER_INVALID_FNUM) {
    UpdateFileHandle(path, *fnum_ptr, conn);
  }
  return {actual_read_size, error};
}

//-----------------------------------------------------------------------------

pair<uint32, Error> SmbProxy::WriteBytesToOffset(const std::string& path,
                                                 SmbConnection::Ptr conn,
                                                 uint64 offset,
                                                 uint32 size,
                                                 const char* data,
                                                 bool cache_handle) {
  DCHECK(conn);

  int32 num_retries = 0;
  uint32 written_size = 0;
  Error error;
  string smb_path = GetSmbPath(path, &conn->context);

  uint16 fnum = SMBC_WRAPPER_INVALID_FNUM;
  uint16* fnum_ptr = nullptr;
  if (cache_handle) {
    fnum_ptr = &fnum;

    // Acquire a shared lock to access LRU cache of connection.
    shared_lock<shared_mutex> lock(conn->mutex);
    // Search in the cache to see whether the file has already been opened.
    auto it = conn->fh_lru_map.find(path);
    if (it != conn->fh_lru_map.end()) {
      DCHECK_NE(it->second->fnum, SMBC_WRAPPER_INVALID_FNUM);
      *fnum_ptr = it->second->fnum;
    }
  }

  while (true) {
    // TODO(Zheng): Right now we are doing immediate retries, because we
    // don't have ClosureRunner and done_cb framework for the GRPC service
    // code. Add a delay for this retry so that it has a better chance to
    // succeed.

    DCHECK_GT(FLAGS_smb_proxy_file_read_write_timeout_secs, 0);
    written_size = smbc_wrapper_write_file_data(
        smb_path.c_str(),
        &conn->context,
        offset,
        size,
        reinterpret_cast<const uint8_t*>(data),
        fnum_ptr,
        FLAGS_smb_proxy_file_read_write_timeout_secs * kNumMsecsInSec);

    if (!written_size) {
      error =
          FromSmbErrorMessage(conn->context.nt_status,
                              "Failed to write data to file " + path +
                                  ", error msg: " + conn->context.error_msg);
      LOG(ERROR) << error.error_detail();
      if (IsConnectionUnusable(conn->context.nt_status)) {
        // If the error is because of the connection has been disconnected,
        // directly return the error so that the caller can retry.
        DCHECK_EQ(error.type(), Error::kRetry);
        return {0, error};
      }

      // We don't retry for non-existent file or access denied file, and we
      // don't update the file handle in such cases.
      if (error.type() == Error::kNonExistent ||
          error.type() == Error::kPermissionDenied) {
        return {0, error};
      }
    } else {
      if (size == written_size) {
        // We have successful written.
        error = Error();
        break;
      }
    }
    DCHECK(!error.Ok());
    ++num_retries;
    if (num_retries <= FLAGS_smb_proxy_file_max_write_retries) {
      LOG(ERROR) << "Retrying writing to file because of error: "
                 << error.DebugString();
    } else {
      break;
    }
  }

  if (cache_handle && *fnum_ptr != SMBC_WRAPPER_INVALID_FNUM) {
    UpdateFileHandle(path, *fnum_ptr, conn);
  }
  return {written_size, error};
}

//-----------------------------------------------------------------------------

void SmbProxy::UpdateFileHandle(const string& path,
                                uint16 fnum,
                                SmbConnection::Ptr conn) {
  DCHECK(!path.empty());
  DCHECK_NE(fnum, SMBC_WRAPPER_INVALID_FNUM);
  DCHECK(conn);

  // Acquire an exclusive lock to update LRU cache of connection.
  unique_lock<shared_mutex> lock(conn->mutex);

  // We need to move this fnum to the end of the LRU list.
  auto it = conn->fh_lru_map.find(path);
  if (it != conn->fh_lru_map.end()) {
    conn->fh_lru_list.erase(it->second);
  }
  FileHandle fh;
  fh.path = path;
  fh.fnum = fnum;
  auto last_it = conn->fh_lru_list.insert(conn->fh_lru_list.end(), move(fh));

  // Now update the LRU map.
  conn->fh_lru_map[path] = move(last_it);

  // Close and remove least recently used file handles until we are not
  // exceeding the maximum file handle size for this connection.
  while (conn->fh_lru_list.size() > FLAGS_smb_proxy_max_fh_cache_size) {
    // Close and remove the least recently used entry.
    const auto& fh = conn->fh_lru_list.front();

    DCHECK_NE(fh.fnum, SMBC_WRAPPER_INVALID_FNUM);
    auto ret = smbc_wrapper_close_fnum(&conn->context, fh.fnum);
    if (ret == -1) {
      // Only log the error message and keep going on.
      LOG(ERROR) << "Failed to close the file handle " << fh.fnum
                 << " for file [" << fh.path << "], connection created at ["
                 << conn->creation_time_usecs
                 << "], reason: " << conn->context.error_msg;
    }

    CHECK(conn->fh_lru_map.erase(fh.path));
    conn->fh_lru_list.pop_front();
    DCHECK_EQ(conn->fh_lru_list.size(), conn->fh_lru_map.size());
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::PurgeFileHandle(const string& task_key, const string& path) {
  // Acquire a shared lock on task_cache_map_ to find task_entry corresponding
  // to the task_key.
  shared_lock<shared_mutex> lock(state_->mutex_);
  TaskCacheEntry::Ptr task_entry;

  auto iter = state_->task_cache_map_.find(task_key);
  if (iter == state_->task_cache_map_.end()) {
    LOG(ERROR) << "Failed to find the cache entry for task " << task_key;
    return;
  } else {
    task_entry = iter->second;
  }
  // Release the lock on task_cache_map_.
  lock.unlock();
  DCHECK(task_entry);

  auto purge_conn_file_handle_fn = [](const string& path,
                                      SmbConnection::Ptr conn) {
    // Acquire an exclusive lock on LRU cache of connection.
    unique_lock<shared_mutex> lock(conn->mutex);
    auto it = conn->fh_lru_map.find(path);
    if (it != conn->fh_lru_map.end()) {
      // Try to close the file handle before purging it.
      auto ret = smbc_wrapper_close_fnum(&conn->context, it->second->fnum);
      if (ret == -1) {
        LOG(ERROR) << "Failed to close the file handle: " << it->second->fnum
                   << " for file: " << path << ", in PurgeFileHandle, reason: "
                   << conn->context.error_msg;
      }
      conn->fh_lru_list.erase(it->second);
      conn->fh_lru_map.erase(it);
    }
  };

  // Acquire a shared lock for this task only.
  shared_lock<shared_mutex> task_lock(task_entry->mutex);

  // Search in the active_connection_map for 'path' file handle.
  for (auto& conn : task_entry->active_connection_map) {
    purge_conn_file_handle_fn(path, conn.second);
  }

  // Search in the inactive_connection_list for 'path' file handle.
  for (auto& conn : task_entry->inactive_connection_list) {
    purge_conn_file_handle_fn(path, conn);
  }
}

//-----------------------------------------------------------------------------

Error SmbProxy::PurgeTaskAndAllConnections(const string& task_key,
                                           TaskCacheEntry::Ptr entry,
                                           bool force_purge) {
  DCHECK(!task_key.empty());
  DCHECK(entry);

  LOG(INFO) << "Purging task " << task_key;

  // Acquire an exclusive lock for this task.
  unique_lock<shared_mutex> task_lock(entry->mutex);

  if (!force_purge && !entry->active_connection_map.empty()) {
    return Error(
        Error::kInvalid,
        "Cannot purge a task while there are active connections being used");
  }

  // Shutdown both active and inactive connections.
  for (const auto& conn : entry->active_connection_map) {
    ShutdownSmbConnection(task_key, entry->share_path, conn.second);
  }
  entry->active_connection_map.clear();

  for (const auto& conn : entry->inactive_connection_list) {
    ShutdownSmbConnection(task_key, entry->share_path, conn);
  }
  entry->inactive_connection_list.clear();

  return Error();
}

//-----------------------------------------------------------------------------

void SmbProxy::MaybePurgeTaskIdleConnections(const string& task_key,
                                             TaskCacheEntry::Ptr entry) {
  DCHECK(!task_key.empty());
  DCHECK(entry);

  const auto now_usecs = GetNowInUsecs();

  // Acquire an exclusive lock for this task.
  unique_lock<shared_mutex> task_lock(entry->mutex);

  for (auto it = entry->inactive_connection_list.begin();
       it != entry->inactive_connection_list.end();) {
    const auto& conn = *it;
    if (now_usecs - conn->last_access_time_usecs >
        FLAGS_smb_proxy_connection_cache_inactive_secs * kNumUsecsInSec) {
      LOG(INFO) << "Purging idle connection created at "
                << conn->creation_time_usecs << " for task " << task_key;
      ShutdownSmbConnection(task_key, entry->share_path, conn);
      it = entry->inactive_connection_list.erase(it);
    } else {
      ++it;
    }
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyCreateEntity(const SmbProxyCreateEntityArg& arg,
                                    SmbProxyCreateEntityResult* result) {
  auto error = ValidateSmbProxyBaseArg(arg.header());
  if (!error.Ok()) {
    result->mutable_error()->CopyFrom(error);
    return;
  }

  auto result_pair = GetOrCreateSmbConnection(arg.header());
  auto conn = result_pair.first;
  if (!conn) {
    CHECK(!result_pair.second.Ok());
    *result->mutable_error() = result_pair.second;
    return;
  }

  DCHECK(arg.has_type());
  DCHECK(arg.has_path());
  DCHECK(!arg.path().empty());

  int return_code = 0;

  string smb_path = GetSmbPath(arg.path(), &conn->context);

  if (arg.type() == EntityType::kFile) {
    return_code = smbc_wrapper_create_object(
        smb_path.c_str(), &conn->context, 1 /* is_file */);
  } else if (arg.type() == EntityType::kDirectory) {
    return_code = smbc_wrapper_create_object(
        smb_path.c_str(), &conn->context, 0 /* is_file */);
  }

  if (return_code == -1) {
    LOG(ERROR) << "Failed to create an entity "<< arg.path()
               << " error msg: "<<  conn->context.error_msg;
    auto error = FromSmbErrorMessage(
        conn->context.nt_status,
        "Failed to create an entity " + arg.path() +
        ", error msg: " + conn->context.error_msg);
    result->mutable_error()->CopyFrom(error);
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyDeleteEntity(const SmbProxyDeleteEntityArg& arg,
                                    SmbProxyDeleteEntityResult* result) {
  DCHECK(arg.has_path());
  DCHECK(!arg.path().empty());
  string path = arg.header().root_dir() + arg.path();
  auto op = DeleteEntityOp::CreatePtr(this,
                                      arg.header(),
                                      move(path),
                                      arg.type(),
                                      arg.exclusive_access());
  auto error = op->Execute();

  if (!error.Ok()) {
    LOG(ERROR) << "DeleteEntityOp failed for " << arg.path()
               << " with error: " << error.ToString();
    *result->mutable_error() = move(error);
  }
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyCreateRestoreFiles(
    const SmbProxyCreateRestoreFilesArg& arg,
    SmbProxyCreateRestoreFilesResult* result) {
  auto op = CreateRestoreFilesOp::CreatePtr(this, arg, result);
  op->Start();
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyWriteFilesData(
    const SmbProxyWriteFilesDataArg& arg,
    SmbProxyWriteFilesDataResult* result) {
  auto op = WriteFilesDataOp::CreatePtr(this, arg, result);
  op->Start();
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyFinalizeRestoreFiles(
    const SmbProxyFinalizeRestoreFilesArg& arg,
    SmbProxyFinalizeRestoreFilesResult* result) {
  auto op = FinalizeRestoreFilesOp::CreatePtr(this, arg, result);
  op->Start();
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyCreateSymlink(const SmbProxyCreateSymlinkArg& arg,
                                     SmbProxyCreateSymlinkResult* result) {
  // First delete the original entity if needed.
  if (arg.delete_entity()) {
    DCHECK(arg.has_path()) << arg.DebugString();
    DCHECK(arg.has_entity_type()) << arg.DebugString();

    string path = arg.header().root_dir() + arg.path();
    auto op = DeleteEntityOp::CreatePtr(this,
                                        arg.header(),
                                        path,
                                        arg.entity_type(),
                                        false /* exclusive access */);
    auto error = op->Execute();
    if (!error.Ok()) {
      LOG(ERROR) << "DeleteEntityOp failed for "
                 << arg.header().root_dir() + arg.path()
                 << " with error: " << error.ToString();
      *result->mutable_error() = move(error);
      return;
    }
  }

  // Next, create the symlink.
  auto op = CreateSymlinkOp::CreatePtr(this, arg, result);
  op->Start();
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyRenameEntity(const SmbProxyRenameEntityArg& arg,
                                    SmbProxyRenameEntityResult* result) {
  auto op = RenameEntityOp::CreatePtr(this, arg, result);
  op->Start();
}

//-----------------------------------------------------------------------------

void SmbProxy::SmbProxyVerifyShare(const SmbProxyVerifyShareArg& arg,
                                   SmbProxyVerifyShareResult* result) {
  auto op = VerifyShareOp::CreatePtr(this, arg, result);
  op->Start();
}

//-----------------------------------------------------------------------------

string SmbProxy::TaskCacheMapString() const {
  string output;
  shared_lock<shared_mutex> global_lock(state_->mutex_);
  output += "Total Entries in Task Cache map : " +
            to_string(state_->task_cache_map_.size()) + "\n\n";
  for (const auto& it : state_->task_cache_map_) {
    output += string(79, '-') + "\n";
    output += "Task key : " + it.first + "\n";
    output += string(79, '-') + "\n";

    const auto& cache_entry = it.second;
    shared_lock<shared_mutex> cache_lock(cache_entry->mutex);
    output += "share_path               : " + cache_entry->share_path + "\n";
    output += "last_access_time_usecs   : " +
              to_string(cache_entry->last_access_time_usecs) + "\n";

    function<string(SmbConnection::Ptr)> SmbConnectionToString =
        [this](SmbConnection::Ptr conn) -> string {
      string output;
      output += "creation_time_usecs      : " +
                to_string(conn->creation_time_usecs) + "\n";
      output += "last_access_time_usecs   : " +
                to_string(conn->last_access_time_usecs) + "\n";
      output +=
          "dir_handle               : " + to_string(conn->dir_handle) + "\n";
      output += "children_fetched         : " +
                to_string(conn->children_fetched) + "\n";

      if (FLAGS_smb_proxy_show_cached_file_handles) {
        shared_lock<shared_mutex> file_handle_lock(conn->mutex);
        output += "Cached file handles      : " +
                  to_string(conn->fh_lru_list.size()) + "\n";
        for (const auto& fh : conn->fh_lru_list) {
          output += "   " + fh.path + " -> " + to_string(fh.fnum) + "\n";
        }
      }
      return output;
    };

    output += "\nActive Connections: " +
              to_string(cache_entry->active_connection_map.size()) + "\n";
    output += string(79, '-') + "\n";
    for (const auto& active_it : cache_entry->active_connection_map) {
      output +=
          "creation_time_usecs      : " + to_string(active_it.first) + "\n";
      output += SmbConnectionToString(active_it.second);
      output += string(79, '-') + "\n";
    }

    output += "\nInactive Connections: " +
              to_string(cache_entry->inactive_connection_list.size()) + "\n";
    output += string(79, '-') + "\n";
    for (const auto& inactive_conn : cache_entry->inactive_connection_list) {
      output += SmbConnectionToString(inactive_conn);
      output += string(79, '-') + "\n";
    }

    output += "\n\n";
  }
  return output;
}

//-----------------------------------------------------------------------------

void SmbProxy::RegisterHTTPHandlers() {
  state_->http_server_->resource["^/$"]["GET"] = [this](
      shared_ptr<HttpServer::Response> response,
      shared_ptr<HttpServer::Request> request) {
    // TODO(akshay): Add general information about SMB proxy.
    response->write("<pre>" + TaskCacheMapString() + "</pre>");
  };
}

//-----------------------------------------------------------------------------

SmbProxy::FileHandlesEntry::Ptr SmbProxy::GetFileHandlesEntry(
    const string& task_key) {
  shared_lock<shared_mutex> s_lock(state_->mutex_);

  auto it = state_->file_handles_map_.find(task_key);
  if (it == state_->file_handles_map_.end()) {
    // The entry does not exist - re-acquire the lock and create it.
    s_lock.unlock();

    // Acquire exclusive lock and insert a new entry.
    unique_lock<shared_mutex> e_lock(state_->mutex_);
    it = state_->file_handles_map_.find(task_key);
    if (it == state_->file_handles_map_.end()) {
      it = state_->file_handles_map_
               .emplace(task_key, make_shared<FileHandlesEntry>())
               .first;
    }
  }
  auto fh_entry = it->second;
  DCHECK(fh_entry) << task_key;
  fh_entry->last_access_time_usecs = GetNowInUsecs();

  return fh_entry;
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
