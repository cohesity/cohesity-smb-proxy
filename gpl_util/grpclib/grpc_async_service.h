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
// This file defines the asynchronous implementation of gRPC server code. Each
// RPC service class needs to be registered in the child implementation of
// RegisterServices(). Each RPC service method needs to implement its own
// version of start_request_fn and process_data_fn, and register itself in the
// child implementation of the RegisterRpcMethods() method. A worker thread can
// call the StartPollingLoop() method to serve requests from clients, and the
// method is thread safe.

#ifndef _GPL_UTIL_GRPCLIB_GRPC_ASYNC_SERVICE_H_
#define _GPL_UTIL_GRPCLIB_GRPC_ASYNC_SERVICE_H_

#include <atomic>
#include <functional>
#include <gflags/gflags.h>
#include <google/protobuf/message.h>
#include <grpc++/grpc++.h>
#include <memory>
#include <mutex>
#include <string>

#include "gpl_util/base/thread_pool.h"

DECLARE_bool(gpl_util_log_grpc_messages);

//-----------------------------------------------------------------------------

// Macro to register an RpcContext object for a specific method. This macro
// should be called within a child class of GrpcAsyncService. The object takes
// two functors, one to start a request with the gRPC service in response to an
// rpc, and another functor to actually execute the method. This macro assumes
// that this cild class has the corresponding function calls for each RPC
// service name it registers.
#define REGISTER_GRPC_METHOD_INNER(name, Arg, Result, service)                \
  do {                                                                        \
    using Service = decltype(service)::element_type;                          \
    using MyContext = RpcContext<Service, Arg, Result>;                       \
                                                                              \
    auto request_fn = [](MyContext* context) {                                \
      /* Call the corresponding method in the gRPC service generated from */  \
      /* foo_rpc.proto */                                                     \
      context->service_->Request##name(&context->grpc_ctx_,                   \
                                       &context->arg_,                        \
                                       &context->responder_,                  \
                                       context->cq_,                          \
                                       context->cq_,                          \
                                       context);                              \
    };                                                                        \
                                                                              \
    auto process_data_fn = [this](MyContext* context) {                       \
      if (FLAGS_gpl_util_log_grpc_messages) {                                 \
        LOG(INFO) << "RPC Arg [" << #name << "]:\n"                           \
                  << context->arg_.DebugString();                             \
        google::FlushLogFiles(google::GLOG_INFO);                             \
      }                                                                       \
                                                                              \
      /* Call the corresponding actualy RPC implementation method */          \
      name(context->arg_, &context->result_);                                 \
                                                                              \
      if (FLAGS_gpl_util_log_grpc_messages) {                                 \
        LOG(INFO) << "RPC Result [" << #name << "]:\n"                        \
                  << context->result_.DebugString();                          \
        google::FlushLogFiles(google::GLOG_INFO);                             \
      }                                                                       \
    };                                                                        \
                                                                              \
    auto inst = new RpcContext<Service, Arg, Result>(                         \
        service.get(), cq_.get(), move(request_fn), move(process_data_fn));   \
                                                                              \
    /* Start listening for requests */                                        \
    inst->Proceed(true);                                                      \
  } while (0)

#define REGISTER_GRPC_METHOD(name, service)                                   \
  REGISTER_GRPC_METHOD_INNER(name, name##Arg, name##Result, service)

//-----------------------------------------------------------------------------

namespace cohesity { namespace gpl_util { namespace grpclib {

// This just serves as a un-templatized wrapper for the RpcContext, so that
// it can be static_cast from a void*.
class Context {
 public:
  virtual ~Context() {}

  // Proceed on the processing of this request service method.
  virtual void Proceed(bool ok) {}
};

//-----------------------------------------------------------------------------

// Context for this RPC call, which contains the necessary data structures to
// serve a request. In addition, it implements a simple state machine for the
// life cycle of serving a request.
template<typename ServiceType, typename ArgType, typename ResultType>
class RpcContext : public Context {
 public:
  // Function for registering a specific RPC method to the completion queue.
  using StartRequestFunctor = std::function<void(RpcContext*)>;

  // Function for actually processing the request and generating the response
  // to be sent to the client.
  using ProcessDataFunctor = std::function<void(RpcContext*)>;

  // Take in the "service" instance (in this case representing an
  // asynchronous server) and the completion queue "cq" used for asynchronous
  // communication with the gRPC runtime.
  RpcContext(ServiceType* service,
             grpc::ServerCompletionQueue* cq,
             StartRequestFunctor start_request_fn,
             ProcessDataFunctor process_data_fn)
      : service_(service),
        cq_(cq),
        start_request_fn_(move(start_request_fn)),
        process_data_fn_(move(process_data_fn)),
        responder_(&grpc_ctx_) {}

  ~RpcContext() {}

  void Proceed(bool ok) override;

  // The means of communication with the gRPC runtime for an asynchronous
  // server. The object always outlives the RPCs.
  ServiceType* service_;

  // The producer-consumer queue where for asynchronous server notifications.
  // The object always outlives the RPCs.
  grpc::ServerCompletionQueue* cq_;

  // gRPC specific Context for the server itself, allowing to tweak aspects
  // of it such as the use of compression, authentication, as well as to send
  // metadata back to the client.
  // TODO(zheng): Investigate whether grpc_ctx_ can be shared among requests.
  grpc::ServerContext grpc_ctx_;

  // Defines the states that an RpcContext can be in. Each state determines
  // what action should be performed the next time Proceed is called:
  // CREATE: Create the context and registering for a specific RPC method.
  // PROCESS: Actually process the request and generate the response, and
  // sending out the response.
  // FINISH: The asynchronous sending is done, so deallocate the context.
  enum CallStatus { CREATE, PROCESS, FINISH };
  CallStatus status_ = CREATE;

  // What we get from the client.
  ArgType arg_;

  // What we send back to the client.
  ResultType result_;

  // The functor to start serving the request of this specific type.
  StartRequestFunctor start_request_fn_;

  // The functor to process the data for the request of this specific type.
  ProcessDataFunctor process_data_fn_;

  // The means to get back to the client.
  grpc::ServerAsyncResponseWriter<ResultType> responder_;

  // A lock to protect the Context object. This is needed as the object may
  // jump from thread to thread between Create and Process phases.
  std::mutex lock_;
};

//-----------------------------------------------------------------------------

// Template function must be defined in .h file.
template <typename ServiceType, typename ArgType, typename ResultType>
void RpcContext<ServiceType, ArgType, ResultType>::Proceed(bool ok) {
  lock_.lock();

  if (status_ == CREATE) {
    // As part of the initial CREATE state, we *request* that the system start
    // processing a specific requests, decided by the virtual StartRequest()
    // implementation. In this request, "this" acts are the tag uniquely
    // identifying the request (so that different Context instances can serve
    // different requests concurrently), in this case the memory address of
    // this Context instance.
    start_request_fn_(this);

    // Make this instance progress to the PROCESS state.
    status_ = PROCESS;
  } else if (status_ == PROCESS) {
    // Create a new Context instance of this type, to serve new requests while
    // waiting for the result to be sent out. The instance will deallocate
    // itself as part of its FINISH state.
    auto ctx =
        new RpcContext(service_, cq_, start_request_fn_, process_data_fn_);

    ctx->Proceed(true);

    if (!ok) {
      lock_.unlock();
      delete this;
      return;
    }

    // Call the user's handler.
    process_data_fn_(this);

    // Let the gRPC runtime know we are ready to sent out the result_ and
    // finish, using the memory address of this instance as the uniquely
    // identifying tag for the event.
    responder_.Finish(result_, grpc::Status::OK, this);

    // Make this instance progress to the FINISH state.
    status_ = FINISH;
  } else {
    // The "result" for this request is already sent out to the client.
    // TODO(zheng): handle the case when (status_ != FINISH);

    // Once in the FINISH state, deallocate ourselves (Context).
    lock_.unlock();
    delete this;
    return;
  }

  lock_.unlock();
}

//-----------------------------------------------------------------------------

class GrpcAsyncService {
 public:
  explicit GrpcAsyncService();
  virtual ~GrpcAsyncService();

  // Starts the server on the given port, creates the completion queue, starts
  // the polling loops in the thread_pool_, and waits for the thread pool work
  // items to finish.
  void StartServer(int32 port);

  // Stops the server and shuts down the completion queue.
  void StopServer();

 protected:
  // This registers all gRPC services (grouped into classes) to the gRPC
  // server builder.
  virtual void RegisterServices(grpc::ServerBuilder* builder) = 0;

  // This creates and registers all RPC methods' call-datas for the main
  // polling loop.
  virtual void RegisterRpcMethods() = 0;

 protected:
  // The main completion queue.
  std::unique_ptr<grpc::ServerCompletionQueue> cq_;

  // The gRPC server.
  std::unique_ptr<grpc::Server> server_;

  // The server address.
  std::string server_address_;

  // Atomic variable to indicate that server has been started or a shutdown has
  // been initiated after it.
  std::atomic_bool server_started_;

  // A thread pool for running the gRPC polling loop.
  std::unique_ptr<ThreadPool> thread_pool_;
};

//-----------------------------------------------------------------------------

} } }  // namespace cohesity, gpl_util, grpclib

#endif  // _GPL_UTIL_GRPCLIB_GRPC_ASYNC_SERVICE_H_
