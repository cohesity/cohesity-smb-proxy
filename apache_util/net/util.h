// Copyright 2019 Cohesity Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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
