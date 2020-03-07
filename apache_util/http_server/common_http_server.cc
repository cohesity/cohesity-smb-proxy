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

#include "apache_util/http_server/common_http_server.h"

#include <gflags/gflags.h>

#ifndef GPERFTOOLS_DISABLE
#include <gperftools/malloc_extension.h>
#include <gperftools/profiler.h>
#include <gperftools/stacktrace.h>
#endif  // GPERFTOOLS_DISABLE

#include <string>
#include <vector>

using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

using std::shared_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace FLAG__namespace_do_not_use_directly_use_DECLARE_int64_instead {
  extern int64 FLAGS_tcmalloc_sample_parameter;
}  // namespace FLAG__namespace_do_not_use_directly_use_DECLARE_int64_instead

//-----------------------------------------------------------------------------

namespace cohesity { namespace apache_util { namespace http_server {

namespace {

// Default tcmalloc sample parameter.
const int64 kDefaultTcmallocSampleParameter = 524288;

//-----------------------------------------------------------------------------

// The HTTP handler for "/flagz".
void FlagzHandler(shared_ptr<HttpServer::Response> response,
                  shared_ptr<HttpServer::Request> request) {
  stringstream ss;
  auto query_fields = request->parse_query_string();
  if (query_fields.empty()) {
    // This is a request to read all gflag values.
    vector<google::CommandLineFlagInfo> all_flags;
    google::GetAllFlags(&all_flags);

    for (const auto& flag : all_flags) {
      ss << "--" << flag.name << "=" << flag.current_value;
      if (!flag.is_default) {
        ss << " [default=" << flag.default_value << "]";
      }
      ss << std::endl;
    }
    response->write(ss.str());
    return;
  }

  // TODO(zheng,akshay): Add handlers for setting or clearing gflags, so that
  // "iris_cli update-gflag" can change gflags for any service that uses a http
  // server created from this file.
}

//-----------------------------------------------------------------------------

#ifndef GPERFTOOLS_DISABLE
// The HTTP handler for "/pprof/heap".
void PprofHeapHandler(shared_ptr<HttpServer::Response> response,
                      shared_ptr<HttpServer::Request> request) {
  auto query_fields = request->parse_query_string();
  auto iter = query_fields.find("TCMALLOC_SAMPLE_PARAMETER");
  if (iter != query_fields.end()) {
    string buf("Ok");
    if (iter->second == "default") {
      FLAG__namespace_do_not_use_directly_use_DECLARE_int64_instead::
          FLAGS_tcmalloc_sample_parameter = kDefaultTcmallocSampleParameter;
    } else {
      try {
        FLAG__namespace_do_not_use_directly_use_DECLARE_int64_instead::
            FLAGS_tcmalloc_sample_parameter = std::stoi(iter->second);
      } catch(...) {
        buf = "Invalid value " + iter->second;
      }
    }
    response->write(buf);
    return;
  }

  string output;
  MallocExtension::instance()->GetHeapSample(&output);
  response->write("<pre>" + output + "</pre>");
}
#endif  // GPERFTOOLS_DISABLE

}  // namespace

//-----------------------------------------------------------------------------

#define REGISTER_GET_HANDLER(uri)                                             \
  http_server->resource[uri]["GET"] = [](                                     \
      shared_ptr<HttpServer::Response> response,                              \
      shared_ptr<HttpServer::Request> request)

//-----------------------------------------------------------------------------

unique_ptr<HttpServer> CreateCommonHttpServer(int32 port) {
  auto http_server = std::make_unique<HttpServer>();
  http_server->config.port = port;

  // Register healthz handler.
  REGISTER_GET_HANDLER("^/healthz$") {
    response->write("Ok");
  };

  // Register flagz handler.
  REGISTER_GET_HANDLER("^/flagz$") {
    FlagzHandler(move(response), move(request));
  };

#ifndef GPERFTOOLS_DISABLE
  // Register heap profiler handler.
  REGISTER_GET_HANDLER("^/pprof/heap$") {
    PprofHeapHandler(move(response), move(request));
  };

  // Set default tcmalloc sample parameter.
  FLAG__namespace_do_not_use_directly_use_DECLARE_int64_instead::
      FLAGS_tcmalloc_sample_parameter = kDefaultTcmallocSampleParameter;

  // Register heapstats handler.
  REGISTER_GET_HANDLER("^/pprof/heapstats$") {
    char stats_buf[4096];
    MallocExtension::instance()->GetStats(stats_buf, sizeof(stats_buf));
    response->write("<pre>" + string(stats_buf) + "</pre>");
  };
#endif  // GPERFTOOLS_DISABLE

  // Register default handler.
  http_server->default_resource["GET"] = [](
      shared_ptr<HttpServer::Response> response,
      shared_ptr<HttpServer::Request> request) {
    *response << "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
  };

  return http_server;
}

//-----------------------------------------------------------------------------

#undef REGISTER_GET_HANDLER

} } }  // namespace cohesity, apache_util, http_server
