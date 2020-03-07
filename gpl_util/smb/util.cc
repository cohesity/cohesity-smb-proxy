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

#include <arpa/inet.h>
#include <boost/filesystem.hpp>
#include <chrono>
#include <functional>
#include <glog/logging.h>
#include <list>
#include <memory>
#include <netdb.h>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>


#include "apache_util/base/scoped_runner.h"
#include "gpl_util/smb/util.h"

using ::cohesity::apache_util::Error;
using ::cohesity::apache_util::ScopedRunner;
using ::cohesity::apache_util::smb::EntityMetadata;
using ::cohesity::apache_util::smb::EntityType;
using ::std::function;
using ::std::list;
using ::std::move;
using ::std::string;

DEFINE_bool(enable_dfs_path,
            true,
            "If set to true, the dfs path is used during create call if the "
            "smb server is dfs capable.");
namespace cohesity { namespace gpl_util { namespace smb {

namespace {

// Helper function to create a directory for a given path. If the directory
// creation fails, it checks if the given path already contains a directory. If
// not, it returns the error.
Error CreateDirectoryWithChecks(const string& path,
                                smbc_wrapper_context* context) {
  auto ret =
      smbc_wrapper_create_object(path.c_str(), context, 0 /* is_file */);
  if (ret < 0) {
    if (context->nt_status == kNtStatusObjectNameCollision) {
      EntityMetadata metadata;
      auto error = GetEntityMetadata(path, context, &metadata);
      if (!error.Ok() || metadata.type() != EntityMetadata::kDirectory) {
        // Existing entity is not a directory.
        return Error(Error::kInvalid,
                     "CreateDirectoryWithChecks failed: Existing entity " +
                         path + " is not a directory");
      }
    } else {
      return FromSmbErrorMessage(
          context->nt_status,
          "Failed on smbc_wrapper_create_object for: " + path +
              ", error_msg: " + context->error_msg);
    }
  }

  return Error();
}

//-----------------------------------------------------------------------------

// Helper function to recursively create the directories in reverse order, to
// minimize GetEntityMetadata() checks required. 'newly_created_dirs' is used
// to record all newly created directories, which will be cleaned up upon
// failures.
Error MkDirWithParentsInternal(const string& path,
                               list<string>* newly_created_dirs,
                               smbc_wrapper_context* context,
                               void* mem_ctx) {
  CHECK(newly_created_dirs);

  // First check whether this path already exists as a directory.
  EntityMetadata metadata;
  auto error = GetEntityMetadata(path, context, mem_ctx, &metadata);

  if (!error.Ok() && error.type() != Error::kNonExistent) {
    return error;
  }

  if (error.Ok()) {
    // The path exists.
    if (metadata.type() != EntityMetadata::kDirectory) {
      return Error(Error::kInvalid,
                   "The specified path: " + path + " is not a directory");
    }
    return error;
  }

  // The specified path does not exist.
  DCHECK(!error.Ok());

  // Recursively create the parent if needed, before trying to create this
  // directory.
  auto pos = path.rfind("\\");

  // All paths should have a leading '\'.
  DCHECK_NE(pos, string::npos) << path;

  auto parent_path = path.substr(0, pos);
  // If pos is 0, it means the parent is "\", which should always exist, so we
  // can skip creating it.
  if (!parent_path.empty()) {
    error = MkDirWithParentsInternal(
        parent_path, newly_created_dirs, context, mem_ctx);
    if (!error.Ok()) {
      return error;
    }
  }

  // We need to call CreateDirectoryWithChecks because of race between multiple
  // threads trying to create same directory.
  // TODO(akshay): Add a global cache of directory creation calls to improve
  // the efficiency and to remove race conditions.
  error = CreateDirectoryWithChecks(path, context);
  if (!error.Ok()) {
    return Error(error.type(),
                 "MkDirWithParentsInternal failed: " + error.error_detail());
  }

  newly_created_dirs->push_back(path);

  // TODO(akshay): Verify that the folder is created with default access
  // permission.

  return Error();
}

}  // namespace

//-----------------------------------------------------------------------------

int64 GetNowInUsecs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

//-----------------------------------------------------------------------------

int64 ConvertTimespecToUsecs(const timespec* ts) {
  return kNumUsecsInSec * ts->tv_sec + ts->tv_nsec / kNumNsecsInUsec;
}

//-----------------------------------------------------------------------------

void FillTimespecFromUsecs(struct timespec* ts, int64 usecs) {
  ts->tv_sec = usecs / kNumUsecsInSec;
  ts->tv_nsec = (usecs % kNumUsecsInSec) * kNumNsecsInUsec;
}

//-----------------------------------------------------------------------------

bool IsConnectionUnusable(uint32 nt_status) {
  return (nt_status == kNtStatusConnectionDisconnected ||
          nt_status == kNtStatusConnectionReset ||
          nt_status == kNtStatusConnectionInvalid ||
          nt_status == kNtStatusConnectionAborted ||
          nt_status == kNtStatusNetworkFileClosed ||
          nt_status == kNtStatusVolumeDismounted ||
          nt_status == kNtStatusInvalidNetworkResponse ||
          nt_status == kNtStatusIOTimeout ||
          nt_status == kNtStatusNetworkNameDeleted ||
          nt_status == kNtStatusNoMemory);
}

//-----------------------------------------------------------------------------

Error FromSmbErrorMessage(uint32 nt_status, string msg) {
  DCHECK(!msg.empty());

  if (nt_status == kNtStatusObjectNameNotFound ||
      nt_status == kNtStatusObjectPathNotFound ||
      nt_status == kNtStatusNotFound ||
      nt_status == kNtStatusNoSuchFile ||
      nt_status == kNtStatusDeletePending) {
    return Error(Error::kNonExistent, move(msg));
  } else if (nt_status == kNtStatusAccessDenied) {
    return Error(Error::kPermissionDenied, move(msg));
  } else if (nt_status == kNtStatusObjectNameCollision) {
    return Error(Error::kAlreadyExists, move(msg));
  } else if (IsConnectionUnusable(nt_status)) {
    // Returning an kRetry error because the proxy is trying to use an unusable
    // connection in the cache, and in such case the client should retry since
    // this is not a failure.
    return Error(Error::kRetry, msg);
  } else if (nt_status == kNtStatusCannotDelete) {
    return Error(Error::kCannotDelete, move(msg));
  } else if (nt_status == kNtStatusPathNotCovered) {
    return Error(Error::kPathNotCovered, move(msg));
  } else {
    return Error(Error::kInvalid, move(msg));
  }
}

//-----------------------------------------------------------------------------

Error GetEntityMetadata(const string& path,
                        smbc_wrapper_context* context,
                        void* mem_ctx,
                        EntityMetadata* md,
                        bool fetch_symlink_tgt) {
  auto* src = smbc_wrapper_get_metadata(path.c_str(), context, mem_ctx);
  if (!src) {
    return FromSmbErrorMessage(context->nt_status,
                               "Failed to get metadata for path " + path +
                               ", error msg: " + context->error_msg);
  }

  if (src->attributes & kSmbAttrDirectory) {
    md->set_type(EntityMetadata::kDirectory);
  } else if (src->attributes & kSmbAttrReparsePoint) {
    // TODO(ashish.puri): See if we could set kSymLink here.
    md->set_type(EntityMetadata::kFile);

    if (fetch_symlink_tgt) {
      uint32_t flags = 0;
      char* target_path = NULL;

      int status = smbc_wrapper_read_symlink(
          path.c_str(), context, mem_ctx, &target_path, &flags);
      if (!status) {
        // Replace '\' with '/'.
        char* current_pos = strchr(target_path, '\\');
        while (current_pos != NULL) {
          *current_pos = '/';
          current_pos = strchr(target_path, '\\');
        }

        md->set_symlink_target(target_path);
        md->set_symlink_flags(flags);
      } else {
        auto error = FromSmbErrorMessage(
            context->nt_status,
            "Failed to get symlink target for path " + path +
                ", error msg: " + context->error_msg);
        LOG(ERROR) << error.error_detail();
        return error;
      }
    }
  } else {
    md->set_type(EntityMetadata::kFile);
  }

  md->set_attributes(src->attributes);
  md->set_uid(src->uid);
  md->set_gid(src->gid);
  md->set_change_time_usecs(ConvertTimespecToUsecs(&src->change_time_ts));
  md->set_modify_time_usecs(ConvertTimespecToUsecs(&src->modify_time_ts));
  md->set_create_time_usecs(ConvertTimespecToUsecs(&src->create_time_ts));
  md->set_access_time_usecs(ConvertTimespecToUsecs(&src->access_time_ts));
  md->set_size(src->size);
  md->set_allocation_size(src->allocation_size);
  md->set_inode_id(src->inode_id);
  md->set_num_hardlinks(src->num_hardlinks);

  auto* ea = md->add_extended_attributes();
  ea->set_name(kSmbXattrACL);
  ea->set_value(string(reinterpret_cast<char*>(src->acls), src->acls_size));

  return Error();
}

//-----------------------------------------------------------------------------

Error GetEntityMetadata(const string& path,
                        smbc_wrapper_context* context,
                        EntityMetadata* md) {
  // Create a memory context to fetch metadata.
  void* mem_ctx =
      smbc_wrapper_talloc_ctx(256 * kNumBytesInKB, "Entity-Metadata");
  if (!mem_ctx) {
    auto error =
        Error(Error::kInvalid,
              "Failed to create memory context for fetching metadata");
    LOG(ERROR) << error.error_detail();
    return error;
  }

  // Cleanup work once this function returns.
  function<void()> cleanup_fn = [mem_ctx]() mutable {
    smbc_wrapper_free(mem_ctx);
  };
  ScopedRunner scoped_runner(move(cleanup_fn));

  return GetEntityMetadata(path, context, mem_ctx, md);
}

//-----------------------------------------------------------------------------

Error MkDirWithParents(const string& path, smbc_wrapper_context* context) {
  if (path.empty() || path[0] != '\\') {
    return Error(Error::kInvalid, "Invalid directory path " + path);
  }

  string dir_path = NormalizePath(path, '\\' /* separator */);

  // Remove trailing slash if present.
  if (!dir_path.empty() && dir_path.back() == '\\') {
    dir_path.pop_back();
  }

  // A list that contains all newly created directories by this function. We
  // use this to only cleanup newly created dirs when this function fails, and
  // we do not remove directories that already existed before this function
  // call.
  list<string> newly_created_dirs;

  // Create a memory context to fetch metadata.
  void* mem_ctx =
      smbc_wrapper_talloc_ctx(256 * kNumBytesInKB, "Entity-Metadata");
  if (!mem_ctx) {
    return Error(Error::kInvalid,
                 "Failed to create memory context for fetching metadata");
  }

  // Cleanup work once this function returns.
  function<void()> cleanup_fn = [mem_ctx]() mutable {
    smbc_wrapper_free(mem_ctx);
  };
  ScopedRunner scoped_runner(move(cleanup_fn));

  auto error = MkDirWithParentsInternal(
      dir_path, &newly_created_dirs, context, mem_ctx);
  // If this function fails, cleanup the newly created directories in reverse
  // order.
  if (!error.Ok()) {
    while (!newly_created_dirs.empty()) {
      const string& dir_path = newly_created_dirs.back();
      int32 ret = smbc_wrapper_delete_directory(dir_path.c_str(), context);
      if (ret < 0) {
        return FromSmbErrorMessage(context->nt_status,
                                   "Failed to delete the directory " +
                                       dir_path + ", error msg: " +
                                       context->error_msg);
      }
      newly_created_dirs.pop_back();
    }
  }

  return error;
}

//-----------------------------------------------------------------------------

Error CreateDirectoryHelper(const string& path,
                            smbc_wrapper_context* context,
                            bool create_parent_dir) {
  // If the given path is empty, return.
  if (path.empty()) {
    return Error();
  }

  Error error;
  if (create_parent_dir) {
    error = MkDirWithParents(path, context);
  } else {
    error = CreateDirectoryWithChecks(path, context);
  }
  return error;
}

//-----------------------------------------------------------------------------

bool HasPrefix(const string& str, const string& prefix) {
  if (prefix.size() > str.size()) {
    return false;
  }
  return std::equal(str.begin(), str.begin() + prefix.size(), prefix.begin());
}

//-----------------------------------------------------------------------------

bool HasSuffix(const string& str, const string& suffix) {
  if (suffix.size() > str.size()) {
    return false;
  }
  auto offset = str.size() - suffix.size();
  return std::equal(str.begin() + offset, str.end(), suffix.begin());
}

//-----------------------------------------------------------------------------

string NormalizePath(const string& path, char separator) {
  if (path.empty()) {
    return path;
  }

  auto normalized = ::boost::filesystem::path(path).normalize().string();
  string separator_str(1, separator);

  // Remove the trailing "/." if present.
  if (HasSuffix(normalized, separator_str + ".")) {
    normalized = normalized.substr(0, normalized.size() - 2);
  }

  // Special handling for the case of "/." (normalized will be empty in this
  // case), "/.." or "//".
  if (normalized.empty() || normalized == separator_str + ".." ||
      normalized == string(2, separator)) {
    normalized = separator_str;
  }

  return normalized;
}

//-----------------------------------------------------------------------------

string GetSmbPath(const string& path,
                  smbc_wrapper_context* context) {
  string smb_path;
  smb_path.reserve(path.size());

  if (FLAGS_enable_dfs_path && context && smbc_wrapper_is_dfs(context)) {
    // Check if path is sanitized already.

    string server = smbc_wrapper_get_server(context);
    string share = smbc_wrapper_get_share(context);
    smb_path = "\\" + server + ((share[0] == '\\') ? share : ("\\" + share));

    // if path starts with smb_path.
    if (strncmp(path.c_str(), smb_path.c_str(), smb_path.size()) == 0) {
      // The path is already sanitized.
      return path;
    }
  }

  bool is_prev_slash = false;
  for (size_t ii = 0; ii < path.size(); ++ii) {
    const auto& char_at = path.at(ii);
    if (char_at == '/' || char_at == '\\') {
      if (!is_prev_slash) {
        smb_path += '\\';
      }
      is_prev_slash = true;
    } else {
      smb_path += char_at;
      is_prev_slash = false;
    }
  }

  return smb_path;
}

//-----------------------------------------------------------------------------

string GetParentDir(const string& path, char separator) {
  DCHECK(!path.empty());
  DCHECK_NE(path.back(), separator);
  const size_t last_separator = path.rfind(separator);
  DCHECK_NE(last_separator, string::npos);
  DCHECK_LT(last_separator + 1, path.size());
  return path.substr(0 /* pos */, last_separator);
}

//-----------------------------------------------------------------------------

// TODO(zheng): Add support for different separator like "\".
bool IsPathChildOf(const string& child_path, const string& parent_path) {
  // We always return false if the parent path is empty.
  if (parent_path.empty()) {
    return false;
  }

  auto child = NormalizePath(child_path);
  auto parent = NormalizePath(parent_path);
  if (child.size() <= parent.size()) {
    return false;
  }

  // If 'parent' is /a/b and 'child' is /a/b_b/c, the latter should not be
  // considered to be a child of the former. However, if the parent is "/",
  // then we don't need to check for this case specially.
  if (child.at(parent.size()) != '/' && parent != "/") {
    return false;
  }

  return HasPrefix(child, parent);
}

//-----------------------------------------------------------------------------

smbc_entity_type ConvertEntityTypeToSmb(EntityType entity_type) {
  // Convert entity_type to the appropriate enum for smbc_wrapper.
  if (entity_type == apache_util::smb::kDirectory) {
    return SMBC_ENTITY_TYPE_DIRECTORY;
  } else if (entity_type == apache_util::smb::kSymLink) {
    return SMBC_ENTITY_TYPE_SYMLINK;
  } else {
    DCHECK_EQ(entity_type, apache_util::smb::kFile);
    return SMBC_ENTITY_TYPE_REGULAR;
  }
}

//-----------------------------------------------------------------------------

EntityType ConvertEntityMetadataTypeToEntityType(
    EntityMetadata::EntityType entity_type) {
  if (entity_type == EntityMetadata::kDirectory) {
    return apache_util::smb::kDirectory;
  } else if (entity_type == EntityMetadata::kSymLink) {
    return apache_util::smb::kSymLink;
  } else {
    DCHECK_EQ(entity_type, EntityMetadata::kFile);
    return apache_util::smb::kFile;
  }
}

//-----------------------------------------------------------------------------

bool IsTargetFileHardlink(const apache_util::smb::EntityMetadata& entity) {
  DCHECK_EQ(entity.type(), apache_util::smb::EntityMetadata::kFile);
  // If the individual files are selected to restore vi GUI, path will be empty
  // and the root dir will be full path to file.
  return entity.num_hardlinks() > 1 && !entity.path().empty();
}

//-----------------------------------------------------------------------------

bool IsIpAddress(const char* server) {
  struct sockaddr_in sa;
  struct sockaddr_in6 sa6;
  return (inet_pton(AF_INET, server, &(sa.sin_addr)) == 1) ||
         (inet_pton(AF_INET6, server, &(sa6.sin6_addr)) == 1);
}

//-----------------------------------------------------------------------------

int CreateSmbConnectionByResolvingHostname(const char* server,
    const char* share,
    const char* username,
    const char* password,
    const char* workgroup,
    smbc_wrapper_context* context) {

    int ret = -1;

    struct addrinfo* ai = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    if (getaddrinfo(server, NULL, &hints, &ai) != 0) {
      return -1;
    }

    struct addrinfo* rp;
    for (rp = ai; rp != NULL; rp = rp->ai_next) {

      char ip_arr[INET6_ADDRSTRLEN];
      const char* ipaddress = NULL;
      struct sockaddr* sa = rp->ai_addr;
      if (sa->sa_family == AF_INET) {
        struct sockaddr_in* sav4 = (struct sockaddr_in*)sa;
        ipaddress =
            inet_ntop(AF_INET, &sav4->sin_addr, ip_arr, INET_ADDRSTRLEN);
      } else if (rp->ai_addr->sa_family == AF_INET6) {
        struct sockaddr_in6* sav6 = (struct sockaddr_in6*)sa;
        ipaddress =
            inet_ntop(AF_INET6, &sav6->sin6_addr, ip_arr, INET6_ADDRSTRLEN);
      } else {
        continue;
      }

      if (ipaddress != NULL) {
        ret = smbc_wrapper_create_connection(
            ipaddress, share, username, password, workgroup, context);
        if (ret != -1) {
          break;
        }
      }
    }

    if (ai) {
      freeaddrinfo(ai);
    }
    return ret;
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
