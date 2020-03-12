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
