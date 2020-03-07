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

#include <chrono>
#include <functional>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpc++/grpc++.h>
#include <memory>

#include "apache_util/base/build_info.h"
#include "apache_util/smb/smb_proxy_rpc.grpc.pb.h"
#include "gpl_util/base/util.h"
#include "gpl_util/smb/smb_proxy.h"

using cohesity::apache_util::smb::SmbProxyGetStatusInfoArg;
using cohesity::apache_util::smb::SmbProxyGetStatusInfoResult;
using cohesity::apache_util::smb::SmbProxyRpcService;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using std::function;
using std::string;

DEFINE_int32(smb_proxy_port, 20002,
             "The port number which the SMB proxy should listen on.");

DECLARE_int32(gpl_util_self_monitoring_sleep_secs);

int main(int argc, char* argv[]) {
  // Get program_name.
  string program_name(argv[0]);
  const size_t last_slash = program_name.rfind('/');
  if (last_slash != string::npos) {
    program_name.erase(0, last_slash + 1);
  }

  // Set the version string.
  google::SetVersionString(string(kBuildVersion) + "\n" +
                           string(program_name.size(), ' ') + "        " +
                           " last-commit-time " + kBuildLastCommitTime);

  google::ParseCommandLineFlags(&argc, &argv, true /* remove_flags */);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // The function to monitor the status of the SMB proxy.
  function<bool()> monitor_fn = []() {
    auto channel = grpc::CreateChannel(
        "localhost:" + std::to_string(FLAGS_smb_proxy_port),
        grpc::InsecureChannelCredentials());
    std::unique_ptr<SmbProxyRpcService::Stub> stub(
        SmbProxyRpcService::NewStub(move(channel)));

    // Set a deadline so that this call does not block forever.
    ClientContext context;
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() +
        std::chrono::seconds(3 * FLAGS_gpl_util_self_monitoring_sleep_secs);
    context.set_deadline(deadline);

    LOG(INFO) << "Checking SMB proxy's status";
    SmbProxyGetStatusInfoArg arg;
    SmbProxyGetStatusInfoResult result;
    Status status = stub->SmbProxyGetStatusInfo(&context, arg, &result);
    if (result.has_error()) {
      LOG(ERROR) << "SmbProxyGetStatusInfo failed with error "
                 << result.error().DebugString();
      return false;
    } else if (!status.ok()) {
      LOG(ERROR) << "SmbProxyGetStatusInfo failed with gRPC error code "
                 << status.error_code() << ": " << status.error_message();
      return false;
    }

    return true;
  };

  cohesity::gpl_util::StartSelfMonitoring(move(program_name),
                                          move(monitor_fn));

  // Only the child process should execute here.
  cohesity::gpl_util::smb::SmbProxy smb_proxy(FLAGS_smb_proxy_port);
  smb_proxy.Start();

  // Sleep in a loop, so that the process does not exit.
  while (true) { sleep(30); };

  return 0;
}
