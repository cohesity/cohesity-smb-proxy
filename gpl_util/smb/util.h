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
// This file contains constants and util functions related to smb proxy.

#ifndef _GPL_UTIL_SMB_UTIL_H_
#define _GPL_UTIL_SMB_UTIL_H_

#include <ctime>
#include <samba/libsmbclient.h>
#include <string>

#include "apache_util/base/error.h"
#include "apache_util/smb/smb_proxy_param.pb.h"

namespace cohesity { namespace gpl_util { namespace smb {

// Number of milli seconds in a second.
const int64 kNumMsecsInSec = 1000;

// How many micro seconds does each second have.
const int64 kNumUsecsInSec = 1000000;

// Number of seconds in a minute.
const int64 kNumSecsInMinute = 60;

// Number of minutes in an hour.
const int64 kNumMinutesInHour = 60;

// Number of micro seconds in an hour.
const int64 kNumUsecsInHour =
    kNumUsecsInSec * kNumSecsInMinute * kNumMinutesInHour;

// How many nano seconds does each micro second have.
const int64 kNumNsecsInUsec = 1000;

// The bit in SMB attributes which indicates that an entity is a directory.
const uint32 kSmbAttrDirectory = 0x00000010;

// The bit in SMB attributes which indicates that an entity is a reparse point
// (symlink).
const uint32 kSmbAttrReparsePoint = 0x00000400;

// SMB attribute which indicates that the file does not have other attributes
// set.
const uint32 kSmbAttrNormal = 0x00000080;

// Information type constants.
const uint32 kInfoTypeFile = 0x01;
const uint32 kInfoTypeSecurity = 0x03;

// Security Descriptors.
const uint32 kOwnerSecurityInformation = 0x00000001;
const uint32 kGroupSecurityInformation = 0x00000002;
const uint32 kDaclSecurityInformation = 0x00000004;

// Bytes related constants.
const uint64 kNumBytesInKB = 1024;
const uint64 kNumBytesInMB = 1024 * kNumBytesInKB;

// NT_STATUS value constants.
const uint32 kNtStatusObjectNameCollision = 0xC0000000 | 0x0035;
const uint32 kNtStatusObjectNameNotFound = 0xC0000000 | 0x0034;
const uint32 kNtStatusObjectPathNotFound = 0xC0000000 | 0x003a;
const uint32 kNtStatusNotFound = 0xC0000000 | 0x0225;
const uint32 kNtStatusNoSuchFile = 0xC0000000 | 0x000f;
const uint32 kNtStatusAccessDenied = 0xC0000000 | 0x0022;
const uint32 kNtStatusCannotDelete = 0xC0000000 | 0x0121;
const uint32 kNtStatusConnectionDisconnected = 0xC0000000 | 0x020c;
const uint32 kNtStatusConnectionReset = 0xC0000000 | 0x020d;
const uint32 kNtStatusConnectionInvalid = 0xC0000000 | 0x023a;
const uint32 kNtStatusConnectionAborted = 0xC0000000 | 0x0241;
const uint32 kNtStatusNetworkNameDeleted = 0xC0000000 | 0x0C9;
const uint32 kNtStatusNetworkFileClosed = 0xC0000000 | 0x0128;
const uint32 kNtStatusVolumeDismounted = 0xC0000000 | 0x026E;
const uint32 kNtStatusInvalidNetworkResponse = 0xC0000000 | 0x00C3;
const uint32 kNtStatusIOTimeout = 0xC0000000 | 0x00B5;
const uint32 kNtStatusDeletePending = 0xC0000000 | 0x0056;
const uint32 kNtStatusPathNotCovered = 0xC0000000 | 0x0257;
const uint32 kNtStatusNoMemory = 0xC0000000 | 0x0017;

// Symlink related constants.
const uint32 kSymlinkFlagRelativePath = 0x00000001;

// The name for the SMB acls to be set for ExtendedAttribute in EntityMetadata.
const char* const kSmbXattrACL = "system.cifs_acl";

// Gets the current time in microseconds since epoch, and returns the value as
// type of int64.
int64 GetNowInUsecs();

// Converts a timespec into the number of micro-seconds since epoch.
int64 ConvertTimespecToUsecs(const timespec* ts);

// Fills timespec struct for given micros-seconds.
void FillTimespecFromUsecs(struct timespec* ts, int64 usecs);

// Returns true iff the nt_status means that the SMB connection is not usable.
bool IsConnectionUnusable(uint32 nt_status);

// Gets an error with the corresponding type based on the error message
// returned from Samba client.
apache_util::Error FromSmbErrorMessage(uint32 nt_status, std::string msg);

// Gets the metadata and extended ACLs for the given path inside the SMB
// share. The result will be stored in 'md'.
apache_util::Error GetEntityMetadata(const std::string& path,
                                     smbc_wrapper_context* context,
                                     void* mem_ctx,
                                     apache_util::smb::EntityMetadata* md,
                                     bool fetch_symlink_tgt = false);

// A helper method to get entity metadata. This function creates memory context
// and calls above method.
apache_util::Error GetEntityMetadata(const std::string& path,
                                     smbc_wrapper_context* context,
                                     apache_util::smb::EntityMetadata* md);

// Creates a directory and also its parent directories as needed. This is
// effectively the same as doing "mkdir -p". Returns kNoError type on success
// and appropriate error on failure. This function does not perform any fsync.
apache_util::Error MkDirWithParents(const std::string& path,
                                    smbc_wrapper_context* context);

// A helper method to create a directory. 'create_parent_dir' indicates if it
// should call MkDirWithParents().
apache_util::Error CreateDirectoryHelper(const std::string& path,
                                         smbc_wrapper_context* context,
                                         bool create_parent_dir);

// Returns true iff 'prefix' is a prefix of 'str'.
bool HasPrefix(const std::string& str, const std::string& prefix);

// Returns true iff 'suffix' is a suffix of 'str'.
bool HasSuffix(const std::string& str, const std::string& suffix);

// Normalizes a path. This function is different from boost's normalize() in
// the way that it removes any trailing "/." from the path, and it also
// converts any path that effectively resolve to "/" (for example, "/.", "/..",
// "//", etc), to "/". If the path is empty, this function also returns an
// empty string.
std::string NormalizePath(const std::string& path, char separator = '/');

// Returns a proper regular SMB path in which all / has been replaced with \,
// and any redundant slashes have been removed.This is necessary because some
// SMB servers (like Isilon) do not like redundant slashes in the path. Notice
// that this function should not be called on an UNC path, which needs
// redundant slashes in it. If context is not null and smb share is dfs
// capable, (\server\share) is prepended to the returned path.
std::string GetSmbPath(const std::string& path, smbc_wrapper_context* context);

// Returns the parent directory of the given path and separator.
std::string GetParentDir(const std::string& path, char separator = '\\');

// Returns true iff 'child_path' is a child of 'parent_path'.
bool IsPathChildOf(const std::string& child_path,
                   const std::string& parent_path);

// Converts EntityType to appropriate enum for smbc_wrapper.
smbc_entity_type ConvertEntityTypeToSmb(
    apache_util::smb::EntityType entity_type);

// Converts EntityMetadata::EntityType to EntityType.
apache_util::smb::EntityType ConvertEntityMetadataTypeToEntityType(
    apache_util::smb::EntityMetadata::EntityType entity_type);

// Helper method to check if the target file to restore is hardlink.
// If single hardlink file in backup is selected for restore in UI, we treat it
// as non-hardlink regular file. This follows from the fact that we do not
// retain hardlinks across root directories while restoring, as DirectoryWalker
// is started separately for each root dir.
bool IsTargetFileHardlink(const apache_util::smb::EntityMetadata& entity);


// Returns true if the server string is ipaddress.
bool IsIpAddress(const char* server);

// Creates a smb connection to server by resolving hostname to ipaddress.
int CreateSmbConnectionByResolvingHostname(const char* server,
                                           const char* share,
                                           const char* username,
                                           const char* password,
                                           const char* workgroup,
                                           smbc_wrapper_context* context);
} } }  // namespace cohesity, gpl_util, smb

#endif  // _GPL_UTIL_SMB_UTIL_H_
