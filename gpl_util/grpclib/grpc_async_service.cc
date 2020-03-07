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
// We are following grpc-1.0.0/example/cpp/helloworld/greeter_async_server.cc
// to implement our asynchronous server.
//
// Each RpcContext object handles exactly one RPC, and can be in one of three
// states: CREATE, PROCESS, or FINISH, and this state determines how the
// Proceed() method will behave.
//
// Before we start the server, we create a template instance of RpcContext for
// each RPC method, and call Proceed on each instance. Suppose we have created
// a "FooContext" instance for some RPC method "Foo" (where FooContext is a
// template instance of RpcContext). When Proceed is called the first time, it
// will register itself with the gRPC server's completion queue. Thus, whenever
// an RPC for the Foo method comes in, the completion queue knows to invoke the
// FooContext instance by calling FooContext.Proceed() again. Once it has
// registered itself, FooText sets its state to PROCEED.
//
// In the StartPollingLoop method, the server will wait for the completion
// queue to detect an incoming RPC. If an RPC for Foo comes in, the completion
// queue will return a tag corresponding to the FooContext object we have
// registered. We can then call Proceed on that object to process the PRC.
//
// When FooContext.Proceed() is called in response to an incoming RPC, it
// handles the request. But first, it creates another instance of FooContext
// and registers the new instance with the completion queue, so that if another
// RPC for the same method comes in, it will be handled by that new
// instance. Then, this FooContext instance will process the request by calling
// a ProcessDataFunctor supplied to FooContext. Currently, this is a
// synchronous blocking call. Once the process is done and FooContext as a
// response to send back, it notifies a gRPC responder to send the response,
// and registers itself with the responder. The responder gRPC returns
// immediately while the response is sent asynchronously to the client.
// FooContext then sets its state to FINISH.
//
// Once gRPC's responder has finished sending the response, the
// StartPollingLoop method will call FooContext.Proceed() one more time. At
// this point, FooContext knows that it can clean up, and thus it will delete
// itself.

#include <arpa/inet.h>
#include <functional>
#include <glog/logging.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>

#include "gpl_util/grpclib/grpc_async_service.h"

using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using std::function;
using std::make_shared;
using std::make_unique;
using std::move;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

DEFINE_bool(gpl_util_log_grpc_messages, false,
            "Whether to log the gRPC arg/result messages.");

DEFINE_int32(gpl_util_grpc_thread_count, 128,
             "The number of threads that we want to use for gRPCs.");

DEFINE_int32(gpl_util_grpc_port_in_use_retry_delay_secs, 1,
             "The number of seconds to wait before retrying for in use "
             "port");

DEFINE_int32(gpl_util_grpc_port_in_use_retry_count, 10,
             "The maximum number of retrying for in use port");

DEFINE_int32(gpl_util_grpc_server_max_receive_message_size,
             20 * 1024 * 1024, /* 20 MB*/
             "The maximum receive message size of the gRPC server.");

//-----------------------------------------------------------------------------

namespace cohesity { namespace gpl_util { namespace grpclib {

namespace {

//-----------------------------------------------------------------------------

// Redirects gRPC log messages to GLog.
void GrpcLogger(gpr_log_func_args *args) {
  switch (args->severity) {
    case GPR_LOG_SEVERITY_DEBUG:
      break;
    case GPR_LOG_SEVERITY_INFO:
      google::LogMessage(args->file, args->line, google::GLOG_INFO).stream()
          << args->message;
      break;
    case GPR_LOG_SEVERITY_ERROR:
      google::LogMessage(args->file, args->line, google::GLOG_ERROR).stream()
          << args->message;
      break;
  }
}

//-----------------------------------------------------------------------------

// Checks whether the port is already used.
bool IsPortInUse(int32 port) {
  auto sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  CHECK_NE(sock, -1) << "The socket() function failed with error: "
                     << strerror(errno);

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  addr.sin_port = htons(port);

  // Try to connect to see if the port is in use.
  bool result =
      connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != -1;

  auto ret = close(sock);
  CHECK_NE(ret, -1) << "The closesocket function failed with error: "
                    << strerror(errno);

  return result;
}

}  // namespace

//-----------------------------------------------------------------------------

GrpcAsyncService::GrpcAsyncService()
    : server_started_(false),
      thread_pool_(make_unique<ThreadPool>(FLAGS_gpl_util_grpc_thread_count)) {
  CHECK(thread_pool_->Init().Ok());
}

//-----------------------------------------------------------------------------

GrpcAsyncService::~GrpcAsyncService() {
  StopServer();
}

//-----------------------------------------------------------------------------

void GrpcAsyncService::StartServer(int32 port) {
  gpr_set_log_function(GrpcLogger);

  // Set the address for the server to listen on.
  server_address_ = "0.0.0.0:" + std::to_string(port);

  // If the port is already in use, retry here for a certain amount of times
  // before giving up.
  int32 retry_count = FLAGS_gpl_util_grpc_port_in_use_retry_count;
  while (IsPortInUse(port)) {
    LOG(INFO) << "Port " << port << " in use, will retry for another "
              << retry_count << " times...";
    sleep(FLAGS_gpl_util_grpc_port_in_use_retry_delay_secs);
    --retry_count;
    if (retry_count == 0) {
      LOG(FATAL) << "Cannot start the gRPC service because the port "
                 << port << " is in use";
    }
  }

  ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(
      FLAGS_gpl_util_grpc_server_max_receive_message_size);
  int selected_port = 0;

  // Right now run the server without a secure channel since it is within our
  // cluster. Add support for secure channel when needed in the future.
  builder.AddListeningPort(
      server_address_, grpc::InsecureServerCredentials(), &selected_port);

  // Register the services through which we'll communicate with clients.
  RegisterServices(&builder);

  // Get hold of the completion queue used for the asynchronous communication
  // with the gRPC runtime.
  cq_ = builder.AddCompletionQueue();

  // Assemble the server.
  server_ = builder.BuildAndStart();

  // Crash the process here when bind() fails internally in gRPC.
  CHECK_NE(selected_port, 0)
      << "Failed to bind a socket to port " << port;

  server_started_ = true;
  LOG(INFO) << "INFO Server listening on " << server_address_;

  // Define the polling function - we are going to use N threads in a pool
  // to run the gRPC poll/dispatch call.
  auto polling_func = [this] {
    RegisterRpcMethods();

    while (true) {
      // Block waiting to read the next event from the completion queue. The
      // event is uniquely identified by its "tag", which in this case is the
      // memory address of a Context instance.
      void* tag;
      bool ok;
      if (!cq_->Next(&tag, &ok)) {
        // Shutting down.
        break;
      }

      if (!ok) {
        VLOG(3) << "Got event from cq, ok is false.";
        if (!server_started_) {
          break;
        }
      }

      // Process the request right here.
      static_cast<Context*>(tag)->Proceed(ok);
    }

    VLOG(3) << "Ended the task.";
  };

  // Consume the dedicated threads.
  for (int32 i = 0; i < FLAGS_gpl_util_grpc_thread_count; ++i) {
    thread_pool_->QueueWork(polling_func);
  }
}

//-----------------------------------------------------------------------------

void GrpcAsyncService::StopServer() {
  if (!server_started_.exchange(false)) {
    LOG(INFO) << "Server stop was already initiated.";
    return;
  }

  LOG(INFO) << "Stoping server " << server_address_;

  if (server_) {
    server_->Shutdown();
    LOG(INFO) << "Stopped server " << server_address_;
  }

  // Always shutdown the completion queue after the server.
  if (cq_) {
    cq_->Shutdown();
    LOG(INFO) << "Server cq was shutdown";
  }

  // Drain completion queue before completion queue (cq_) is destroyed.
  VLOG(1) << "Draining server completion queue.";
  void* ignored_tag;
  bool ignored_ok;
  while (cq_->Next(&ignored_tag, &ignored_ok)) {
    // Draining queue, ignore tag.
    auto ctx = static_cast<Context*>(ignored_tag);
    delete ctx;
  }
  VLOG(1) << "Server completion queue drained.";
}

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, grpclib
