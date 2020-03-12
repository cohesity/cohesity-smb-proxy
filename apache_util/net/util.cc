// Copyright (C) 2019-2020 Cohesity, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// Author: Zheng Cai

#include "apache_util/net/util.h"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <netdb.h>

#include "apache_util/base/basictypes.h"

using ::std::string;
using ::std::unordered_set;

DEFINE_int32(apache_util_net_dns_resolve_num_attempts, 10,
             "The number of DNS resolution attempts to find all possible "
             "resolved IPv4 addresses for a given hostname.");

namespace cohesity { namespace apache_util { namespace net {

namespace {

// Pre-computed table used to eliminate integer division by 10, which is slow
// on contemporary binary CPUs. See the following page for benchmarks
// (MIT license):
//    https://github.com/miloyip/itoa-benchmark
const char kDigitTable[200] = {
    '0', '0', '0', '1', '0', '2', '0', '3', '0', '4', '0', '5', '0', '6', '0',
    '7', '0', '8', '0', '9', '1', '0', '1', '1', '1', '2', '1', '3', '1', '4',
    '1', '5', '1', '6', '1', '7', '1', '8', '1', '9', '2', '0', '2', '1', '2',
    '2', '2', '3', '2', '4', '2', '5', '2', '6', '2', '7', '2', '8', '2', '9',
    '3', '0', '3', '1', '3', '2', '3', '3', '3', '4', '3', '5', '3', '6', '3',
    '7', '3', '8', '3', '9', '4', '0', '4', '1', '4', '2', '4', '3', '4', '4',
    '4', '5', '4', '6', '4', '7', '4', '8', '4', '9', '5', '0', '5', '1', '5',
    '2', '5', '3', '5', '4', '5', '5', '5', '6', '5', '7', '5', '8', '5', '9',
    '6', '0', '6', '1', '6', '2', '6', '3', '6', '4', '6', '5', '6', '6', '6',
    '7', '6', '8', '6', '9', '7', '0', '7', '1', '7', '2', '7', '3', '7', '4',
    '7', '5', '7', '6', '7', '7', '7', '8', '7', '9', '8', '0', '8', '1', '8',
    '2', '8', '3', '8', '4', '8', '5', '8', '6', '8', '7', '8', '8', '8', '9',
    '9', '0', '9', '1', '9', '2', '9', '3', '9', '4', '9', '5', '9', '6', '9',
    '7', '9', '8', '9', '9'};

//-----------------------------------------------------------------------------

// Converts a single u8 value to ASCII and appends it to the given buffer.
void u8toa(uint8 value, char** buffer) {
  const uint32 d1 = (value / 100) << 1;
  const uint32 d2 = (value % 100) << 1;

  auto& ptr = *buffer;
  if (value >= 100) *ptr++ = kDigitTable[d1 + 1];
  if (value >= 10) *ptr++ = kDigitTable[d2];
  *ptr++ = kDigitTable[d2 + 1];
}

//-----------------------------------------------------------------------------

// A local equivalent of inet_ntoa() that avoids sprintf() for perf reasons.
string IPv4AddrToString(const in_addr& in) {
  string result(15, '\0');

  auto* bytes = reinterpret_cast<const uint8*>(&in);

  char* out = &result[0];
  u8toa(bytes[0], &out);
  *out++ = '.';
  u8toa(bytes[1], &out);
  *out++ = '.';
  u8toa(bytes[2], &out);
  *out++ = '.';
  u8toa(bytes[3], &out);

  result.resize(out - &result[0]);
  return result;
}

}  // namespace

//-----------------------------------------------------------------------------

unordered_set<string> MaybeResolveIPv4(const string& hostname_or_ipaddr) {
  unordered_set<string> result_set;

  addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_socktype = SOCK_STREAM;

  for (int32 ii = 0; ii < FLAGS_apache_util_net_dns_resolve_num_attempts;
       ++ii) {
    struct addrinfo* result;
    int32 error =
        getaddrinfo(hostname_or_ipaddr.c_str(), nullptr, &hints, &result);
    if (error != 0) {
      LOG(WARNING) << "getaddrinfo for " << hostname_or_ipaddr
                   << " failed with error: " << gai_strerror(error);
      return result_set;
    }
    CHECK_EQ(result->ai_family, AF_INET);

    const auto ip4_addr =
        reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    result_set.emplace(IPv4AddrToString(ip4_addr));
  }

  return result_set;
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, apache_util, net
