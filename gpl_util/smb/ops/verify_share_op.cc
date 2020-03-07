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
// Author: Zheng Cai

#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/smb/ops/verify_share_op.h"

#include <glog/logging.h>
#include <string>

#include "apache_util/net/util.h"
#include "gpl_util/base/util.h"
#include "gpl_util/smb/util.h"

using ::cohesity::apache_util::Error;
using ::cohesity::apache_util::smb::SmbProxyVerifyShareArg;
using ::cohesity::apache_util::smb::SmbProxyVerifyShareResult;
using ::std::move;
using ::std::string;

DEFINE_int32(smb_proxy_verify_share_retries, 3,
             "The number of retries to perform when none of the resolved IPs "
             "can be used to connect to the target share.");

namespace cohesity { namespace gpl_util { namespace smb {

//-----------------------------------------------------------------------------

SmbProxy::VerifyShareOp::Ptr SmbProxy::VerifyShareOp::CreatePtr(
    SmbProxy* owner,
    const SmbProxyVerifyShareArg& arg,
    SmbProxyVerifyShareResult* result) {
  return Ptr(new VerifyShareOp(owner, arg, result));
}

//-----------------------------------------------------------------------------

SmbProxy::VerifyShareOp::VerifyShareOp(SmbProxy* owner,
                                       const SmbProxyVerifyShareArg& arg,
                                       SmbProxyVerifyShareResult* result)
    : owner_(owner), arg_(arg), result_(result) {
  DCHECK(owner_);
  DCHECK(result_);
}

//-----------------------------------------------------------------------------

void SmbProxy::VerifyShareOp::Start() {
  const auto& header = arg_.header();
  auto error = owner_->ValidateSmbProxyBaseArg(header);
  if (!error.Ok()) {
    *result_->mutable_error() = move(error);
    return;
  }

  // First get the resolved IPv4 address(es) for the SMB server, which we will
  // verify and find the first one available for us to access the share.
  auto ipv4_addr_set = apache_util::net::MaybeResolveIPv4(header.server());
  if (ipv4_addr_set.empty()) {
    error = Error(Error::kInvalid, "Cannot resolve " + header.server());
  }

  for (int32 ii = 0; ii <= FLAGS_smb_proxy_verify_share_retries; ++ii) {
    for (const auto& addr : ipv4_addr_set) {
      auto ip_header = header;
      ip_header.set_server(addr);

      auto result_pair = owner_->GetOrCreateSmbConnection(ip_header);
      auto conn = result_pair.first;
      if (!conn) {
        // This IP cannot be used to establish the connection, save the error
        // and move to the next IP.
        DCHECK(!result_pair.second.Ok());
        error = move(result_pair.second);
        continue;
      }

      // We found a valid IP to establish the connection, now get the size
      // information about this share.
      DCHECK(conn);
      auto size_info = smbc_wrapper_get_share_size_info(&conn->context);
      if (!size_info.success) {
        error = FromSmbErrorMessage(
            conn->context.nt_status,
            StringPrintf(
                "Failed to query size info for share %s, error msg: %s",
                (header.server() + header.share()).c_str(),
                conn->context.error_msg));
      }

      // Shutdown the connection because it may not be used again.
      owner_->ShutdownSmbConnection(
          ip_header.task_key(), ip_header.server() + ip_header.share(), conn);
      owner_->ReleaseSmbConnection(ip_header, move(conn), false /* reuse */);

      if (!error.Ok()) {
        // Retry with next IP if still available.
        continue;
      }

      // Set the result fields.
      result_->set_first_working_ip(addr);
      result_->set_total_allocation_units(size_info.total_allocation_units);
      result_->set_caller_available_allocation_units(
          size_info.caller_available_allocation_units);
      result_->set_actual_available_allocation_units(
          size_info.actual_available_allocation_units);
      result_->set_sectors_per_allocation_unit(
          size_info.sectors_per_allocation_unit);
      result_->set_bytes_per_sector(size_info.bytes_per_sector);

      // Calculate logical size and available size in bytes.
      const int64 size_bytes_per_unit =
          static_cast<int64>(size_info.sectors_per_allocation_unit) *
          static_cast<int64>(size_info.bytes_per_sector);
      result_->set_logical_size_bytes(size_info.total_allocation_units *
                                      size_bytes_per_unit);
      result_->set_available_size_bytes(
          size_info.actual_available_allocation_units * size_bytes_per_unit);
      return;
    }
  }

  // Cannot succeed after retries.
  DCHECK(!error.Ok());
  *result_->mutable_error() = move(error);
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, smb
