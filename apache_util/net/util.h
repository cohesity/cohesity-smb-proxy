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
//
// This file contains various networking utility functions.

#ifndef _APACHE_UTIL_NET_UTIL_H_
#define _APACHE_UTIL_NET_UTIL_H_

#include <string>
#include <unordered_set>

namespace cohesity { namespace apache_util { namespace net {

// Resolve a hostname to IPv4 address(es). If an IPv4 address is passed instead
// of a hostname, this function will effectively be a no-op and return the same
// IPv4 address. This returns an empty set if the DNS resolution fails. If the
// hostname resolves to multiple different IPv4 addresses, it returns all of
// them in the set.
std::unordered_set<std::string> MaybeResolveIPv4(
    const std::string& hostname_or_ipaddr);

} } }  // namespace cohesity, apache_util, net

#endif  // _APACHE_UTIL_NET_UTIL_H_
