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
// Author: Akshay Hebbar Yedagere Sudharshana (akshay@cohesity.com)
//
// This file defines utility to create a common http server.

#ifndef _APACHE_UTIL_HTTP_SERVER_COMMON_HTTP_SERVER_H_
#define _APACHE_UTIL_HTTP_SERVER_COMMON_HTTP_SERVER_H_

#include <algorithm>
#include <memory>
#include <simple_web_server/server_http.hpp>
#include <string>

#include "apache_util/base/basictypes.h"

namespace cohesity { namespace apache_util { namespace http_server {

using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

// This function creates a http server which listens on "port", and has
// registered common handlers, including /healthz, /flagz, /pprof/heap, and
// /pprof/heapstats. The holder of this server should invoke its start()
// function to get the server started.
std::unique_ptr<HttpServer> CreateCommonHttpServer(int32 port);

} } }  // namespace cohesity, apache_util, http_server

#endif  // _APACHE_UTIL_HTTP_SERVER_COMMON_HTTP_SERVER_H_
