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
// This file defines the implementation of the SMB proxy.

#ifndef _GPL_UTIL_SMB_SMB_PROXY_H_
#define _GPL_UTIL_SMB_SMB_PROXY_H_

#include <boost/thread/shared_mutex.hpp>
#include <list>
#include <memory>
#include <samba/libsmbclient.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "gpl_util/base/id_locker.h"
#include "gpl_util/base/lru_cache.h"
#include "gpl_util/grpclib/grpc_async_service.h"

namespace cohesity {
  namespace apache_util { namespace smb {
  class SmbProxyBaseArg;
  class SmbProxyGetStatusInfoArg;
  class SmbProxyGetStatusInfoResult;
  class SmbProxyGetRootMetadataArg;
  class SmbProxyGetRootMetadataResult;
  class SmbProxyFetchChildrenMetadataArg;
  class SmbProxyFetchChildrenMetadataResult;
  class SmbProxyFetchFileDataArg;
  class SmbProxyFetchFileDataResult;
  class SmbProxyWriteFileDataArg;
  class SmbProxyWriteFileDataResult;
  class SmbProxyPurgeTaskArg;
  class SmbProxyPurgeTaskResult;
  class SmbProxyCreateEntityArg;
  class SmbProxyCreateEntityResult;
  class SmbProxyDeleteEntityArg;
  class SmbProxyDeleteEntityResult;
  class SmbProxyCreateRestoreFilesArg;
  class SmbProxyCreateRestoreFilesResult;
  class SmbProxyWriteFilesDataArg;
  class SmbProxyWriteFilesDataResult;
  class SmbProxyFinalizeRestoreFilesArg;
  class SmbProxyFinalizeRestoreFilesResult;
  class SmbProxyCreateSymlinkArg;
  class SmbProxyCreateSymlinkResult;
  class SmbProxyRenameEntityArg;
  class SmbProxyRenameEntityResult;
  class SmbProxyVerifyShareArg;
  class SmbProxyVerifyShareResult;
  } }  // namespace apache_util, smb
  namespace gpl_util {
  class ThreadPool;
  }  // namespace gpl_util
}  // namespace cohesity

namespace cohesity { namespace gpl_util { namespace smb {

class SmbProxy : public grpclib::GrpcAsyncService {
 public:
  typedef std::shared_ptr<SmbProxy> Ptr;
  typedef std::shared_ptr<const SmbProxy> PtrConst;

  // Ctor and dtor.
  explicit SmbProxy(int32 rpc_port);
  SmbProxy(int32 rpc_port, int32 http_port);
  ~SmbProxy() override;

  // Starts the SMB Proxy service.
  void Start();

 protected:
  // Method that runs the alarm handler. This monitors the status of all cache
  // entries, and purges any one that has not been used in a long time.
  void AlarmHandler();

  // This registers all the gRPC services to the gRPC server.
  void RegisterServices(grpc::ServerBuilder* builder) override;

  // This registers all RPC methods of each gRPC service to the gRPC server.
  void RegisterRpcMethods() override;

  // Gets the basic status info about the SMB proxy.
  void SmbProxyGetStatusInfo(
      const apache_util::smb::SmbProxyGetStatusInfoArg& arg,
      apache_util::smb::SmbProxyGetStatusInfoResult* result);

  // Gets the metadata of the root directory node or the path for the specified
  // SMB share in 'arg'. Returns the metadata. Any error that occurs during
  // this call will also be populated in 'result'.
  void SmbProxyGetRootMetadata(
      const apache_util::smb::SmbProxyGetRootMetadataArg& arg,
      apache_util::smb::SmbProxyGetRootMetadataResult* result);

  // Gets the metadata of all the children (excluding '.' and '..') for the
  // specified directory in the SMB share. Returns the vector of metadata. The
  // initial call should pass an empty cookie in the arg. If there are more
  // remaining children to be fetched, then the result will contain a non-empty
  // cookie, which should be used in the next call of
  // SmbProxyGetRootMetadata. Any error that occurs during this call will also
  // be populated in 'result'.
  void SmbProxyFetchChildrenMetadata(
      const apache_util::smb::SmbProxyFetchChildrenMetadataArg& arg,
      apache_util::smb::SmbProxyFetchChildrenMetadataResult* result);

  // Fetches the data for a file at the given offset. Any error that occurs
  // during this call will also be populated in 'result'.
  void SmbProxyFetchFileData(
      const apache_util::smb::SmbProxyFetchFileDataArg& arg,
      apache_util::smb::SmbProxyFetchFileDataResult* result);

  // Writes the data to a file at the given offset. Any error that occurs
  // during this call will also be populated in 'result'.
  void SmbProxyWriteFileData(
      const apache_util::smb::SmbProxyWriteFileDataArg& arg,
      apache_util::smb::SmbProxyWriteFileDataResult* result);

  // Purges open connections and cached files handles for a task.
  void SmbProxyPurgeTask(const apache_util::smb::SmbProxyPurgeTaskArg& arg,
                         apache_util::smb::SmbProxyPurgeTaskResult* result);

  // Creates a file or a directory.
  void SmbProxyCreateEntity(
      const apache_util::smb::SmbProxyCreateEntityArg& arg,
      apache_util::smb::SmbProxyCreateEntityResult* result);

  // Deletes a file or a directory.
  void SmbProxyDeleteEntity(
      const apache_util::smb::SmbProxyDeleteEntityArg& arg,
      apache_util::smb::SmbProxyDeleteEntityResult* result);

  // Creates files to be retored.
  void SmbProxyCreateRestoreFiles(
      const apache_util::smb::SmbProxyCreateRestoreFilesArg& arg,
      apache_util::smb::SmbProxyCreateRestoreFilesResult* result);

  // Writes data to a set of files which are being restored.
  void SmbProxyWriteFilesData(
      const apache_util::smb::SmbProxyWriteFilesDataArg& arg,
      apache_util::smb::SmbProxyWriteFilesDataResult* result);

  // Finalizes restored files/directories.
  void SmbProxyFinalizeRestoreFiles(
      const apache_util::smb::SmbProxyFinalizeRestoreFilesArg& arg,
      apache_util::smb::SmbProxyFinalizeRestoreFilesResult* result);

  // Creates a symlink (reparse point) in the SMB share.
  void SmbProxyCreateSymlink(
      const apache_util::smb::SmbProxyCreateSymlinkArg& arg,
      apache_util::smb::SmbProxyCreateSymlinkResult* result);

  // Renames an entity (file, symlink, directory) in the SMB share.
  void SmbProxyRenameEntity(
      const apache_util::smb::SmbProxyRenameEntityArg& arg,
      apache_util::smb::SmbProxyRenameEntityResult* result);

  // Verifies the specified SMB share can be connected to using the provided
  // credentials, and returns the size information about the share.
  void SmbProxyVerifyShare(
      const apache_util::smb::SmbProxyVerifyShareArg& arg,
      apache_util::smb::SmbProxyVerifyShareResult* result);

 private:
  // Op that creates the files to be restored.
  class CreateRestoreFilesOp;

  // Op that deletes an entity.
  class DeleteEntityOp;

  // Op that writes data to multiple files.
  class WriteFilesDataOp;

  // Op that finalizes restored files.
  class FinalizeRestoreFilesOp;

  // Op that creates SMB symlink.
  class CreateSymlinkOp;

  // Op that renames an entity.
  class RenameEntityOp;

  // Op that fetches cached file handle for a given path.
  class FetchFileHandleOp;

  // Op that verifies a SMB share.
  class VerifyShareOp;

 private:
  // An entry for a file handle, including its path and fnum.
  struct FileHandle {
    std::string path;
    uint16 fnum;
  };

  // The data structure for one SMB connection and the directory handles opened
  // within that connection. Because libsmbclient is not thread-safe for each
  // connection, this structure should only be held and used by one thread at
  // the same time.
  struct SmbConnection {
    typedef std::shared_ptr<SmbConnection> Ptr;
    typedef std::shared_ptr<const SmbConnection> PtrConst;

    // The time in usecs when this connection was created. This is used to
    // uniquely identify a connection. This will also be used as the cookie in
    // SmbProxyFetchChildrenMetadataArg and
    // SmbProxyFetchChildrenMetadataResult.
    int64 creation_time_usecs;

    // The time (in usecs) when this connection was accessed.
    int64 last_access_time_usecs;

    // The connection wrapper context.
    smbc_wrapper_context context;

    // The directory handle currently opened within this connection. This is
    // used to track and purge inactive directory handle left by a previously
    // crashed client, and also to ensure that the directory handle given by
    // the client is valid (in case the proxy crashes and restarts).
    uint16 dir_handle = SMBC_WRAPPER_INVALID_FNUM;

    // How many children have been fetched for the directory of dir_handle so
    // far. Only valid when dir_handle is not SMBC_WRAPPER_INVALID_FNUM. This
    // counter is used to ensure that the client is in sync with the server. If
    // the counters do not match, it means that the client has failed to
    // receive some children in previous calls, and in such case the client
    // needs to retry from the beginning.
    int64 children_fetched = 0;

    // Mutex for protecting fh_lru_list and fh_lru_map;
    boost::shared_mutex mutex;

    // LRU list of cached file handle's path. Objects accessed earlier are at
    // the beginning of the list.
    std::list<FileHandle> fh_lru_list;

    // A map from a file path to the iterator of the list node storing the file
    // handle opened in this connection. Together with 'fh_lru_list' this
    // implements a LRU cache.
    std::unordered_map<std::string, std::list<FileHandle>::iterator>
        fh_lru_map;
  };

  // The cache entry for one task.
  struct TaskCacheEntry {
    typedef std::shared_ptr<TaskCacheEntry> Ptr;

    // Mutex for protecting the shared objects below.
    boost::shared_mutex mutex;

    // The time (in usecs) when this cache entry was accessed.
    int64 last_access_time_usecs;

    // The full SMB share path.
    std::string share_path;

    // The map for actively used SMB share connections. The key is the
    // connection's creation time, and the value is the connection structure.
    std::unordered_map<int64, SmbConnection::Ptr> active_connection_map;

    // The list for inactive SMB share connections. We reuse previous
    // connections so that we don't have to recreate a new connection for every
    // single RPC call, to improve performance.
    std::list<SmbConnection::Ptr> inactive_connection_list;
  };

  // Validates the SMB proxy base argument passed in. Returns an error iff
  // there is any.
  apache_util::Error ValidateSmbProxyBaseArg(
      const apache_util::smb::SmbProxyBaseArg& arg) const;

  // Updates the last access time of a task.
  void UpdateTaskAccessTime(const std::string& task_key);

  // if 'handle_path' is not empty, then this function will go through inactive
  // connections to find the one that has the cached handle for the given
  // path. If none of such connection can be found, then the first inactive
  // connection in the list will be returned. If there is no more inactive
  // connection, a new connection will be created. (nullptr,
  // Error(Error::kInvalid, details) will be returned if the connection fails
  // to be opened.
  //
  // If 'creation_time_usecs' is not -1, 'children_fetched' also needs to be
  // provided. This function gets the SMB connection uniquely identified by
  // 'creation_time_usecs' in the active_connection_map of the task. Returns
  // (nullptr, Error(Error::kNonExistent, Error::kCookieNonExistentErrorMsg))
  // if the connection cannot be found, or 'children_fetched' does not match
  // the value stored in the connection entry.
  //
  // If 'creation_time_usecs' is -1, this function first tries to find an
  // inactive connection from this task and returns it. If there is no more
  // inactive connection, a new connection will be created.
  //
  // 'handle_path' should not be provided together with 'creation_time_usecs'
  // and 'children_fetched'.
  std::pair<SmbConnection::Ptr, apache_util::Error> GetOrCreateSmbConnection(
      const apache_util::smb::SmbProxyBaseArg& arg,
      const std::string handle_path = "",
      int64 creation_time_usecs = -1,
      int64 children_fetched = 0);

  // Removes the connection from the task's active connection map. If 'reuse'
  // is true, also moves it to the task's inactive connection map, so that it
  // can be reused by later RPC calls of this task.
  void ReleaseSmbConnection(const apache_util::smb::SmbProxyBaseArg& arg,
                            SmbConnection::Ptr conn,
                            bool reuse = true);

  // Shuts down the connection of the task.
  void ShutdownSmbConnection(const std::string& task_key,
                             const std::string& share_path,
                             SmbConnection::Ptr conn);

  // Reads 'size' bytes at 'offset' from the file of 'path' using the
  // connection 'conn'. The bytes read are copied to the buffer 'buf' starting
  // at buf_offset. The caller has to make sure that 'size' is no larger than
  // the 'max_read_size' negotiated in this connection. Otherwise this function
  // will fail with an error. Returns the actual number of bytes read, and any
  // error encountered in this call. This function retries multiple times upon
  // an error. Notice that, when this function reads fewer bytes than
  // requested, it still copies whatever it has read to 'buf' and returns an
  // error. If 'cache_handle' is true, then this function will try to reuse the
  // handle (fnum) opened for this file if already cached, or cache the handle
  // if it needs to be opened. Otherwise, a new handle will be opened and
  // closed for each call.
  std::pair<uint32, apache_util::Error> ReadBytesFromOffset(
      const std::string& path,
      SmbConnection::Ptr conn,
      uint64 offset,
      uint32 size,
      std::string* buf,
      uint32 buf_offset,
      bool cache_handle = false);

  // Writes data to file at offset. This function retries multiple times upon
  // an error. If 'cache_handle' is true, then this function will try to resuse
  // the handle (fnum) opened for this file if already cached, or cache the
  // handle if it needs to be opened. Otherwise, a new handle will be opened
  // and closed for each call.
  std::pair<uint32, apache_util::Error> WriteBytesToOffset(
      const std::string& path,
      SmbConnection::Ptr conn,
      uint64 offset,
      uint32 size,
      const char* data,
      bool cache_handle = false);

  // A helper method to write data to a given file. This function handles
  // writes of size greater than max_write_size of a SMB connection.
  apache_util::Error WriteFileDataHelper(const std::string& path,
                                         SmbConnection::Ptr conn,
                                         const std::string& data,
                                         int64 size,
                                         int64 offset,
                                         bool cache_handle = false,
                                         bool is_stubbed = false);

  // Updates the file handle 'fnum' in the LRU cache of connection
  // 'conn'. Since more recently used entries are at the end in the cache, the
  // entry for 'fnum' will be moved to the end. If the cache is full, the least
  // recently used entry will be removed.
  void UpdateFileHandle(const std::string& path,
                        uint16 fnum,
                        SmbConnection::Ptr conn);

  // Purges the file handle of 'path' by iterating over all the connections of
  // given 'task_key'. This is especially useful when the client side removes a
  // file and recreates the same file, in which case the previous cached handle
  // for that file will be invalid, and should be purged from the cache.
  void PurgeFileHandle(const std::string& task_key, const std::string& path);

  // Purges all open connections and cached files handles for a task. If
  // 'force_purge' is true (usually called by the alarm handler when the task
  // is idle for too long), then this purges both active and inactive
  // connections. Otherwise (usually called by an RPC), this function checks
  // whether there are still active connections, and if so, it fails with an
  // error, because we cannot purge connections that are actively being used.
  apache_util::Error PurgeTaskAndAllConnections(const std::string& task_key,
                                                TaskCacheEntry::Ptr entry,
                                                bool force_purge = false);

  // Checks and purges any inactive connections that are idle for longer than
  // FLAGS_smb_proxy_connection_cache_inactive_secs, to prevent
  // NT_STATUS_CONNECTION_DISCONNECTED errors.
  void MaybePurgeTaskIdleConnections(const std::string& task_key,
                                     TaskCacheEntry::Ptr entry);

  // A helper method to register all the HTTP handlers.
  void RegisterHTTPHandlers();

  // A helper method to serialize the task_cache_map_ to string.
  std::string TaskCacheMapString() const;

  // A wrapper struct containing a cache of file handles for a task. This is
  // global for a task unlike SmbConnection's cache which is specific to the
  // given connection. This is needed because the new restore flow sends
  // CreateRestoreFiles RPC for each write. So we need a way to know if a file
  // is already created. This will be used for that purpose only.
  struct FileHandlesEntry {
    typedef std::shared_ptr<FileHandlesEntry> Ptr;

    // Ctor.
    FileHandlesEntry();

    // Mutex for protecting the shared objects below.
    boost::shared_mutex mutex;

    // The time (in usecs) when this FileHandlesEntry instance was accessed.
    int64 last_access_time_usecs;

    // Struct containing information about a file handle.
    struct FileHandleEntry {
      // Any error encountered while opening the file.
      apache_util::Error error;

      // The timestamp (in usecs) when we encountered any error while opening
      // the file.
      int64 error_time_usecs = -1;
    };

    // A cache from file path to the file handle entry.
    LRUCache<std::string, FileHandleEntry> fh_cache;
  };

  // Fetches the FileHandlesEntry for task_key, creates if not already present
  // in file_handles_map_.
  std::shared_ptr<FileHandlesEntry> GetFileHandlesEntry(
      const std::string& task_key);

  // A helper function to initialize class members.
  void Init(int32 rpc_port, int32 http_port);

  IDLocker<std::string>& id_locker();

 private:
  // Conceal the state so that Boost ASIO headers do not leak into the world.
  struct State;
  std::unique_ptr<State> state_;
};

} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_SMB_PROXY_H_
