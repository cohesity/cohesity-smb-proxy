# Copyright 2017 Cohesity Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Author: Zheng Cai

proto_generate_cpp(smb_proxy_param.proto)
proto_generate_cpp(smb_proxy_rpc.proto)
proto_generate_grpc_cpp(smb_proxy_rpc.proto)

add_libraries(apache_util_smb_proxy_rpc_proto
    smb_proxy_param.pb.cc
    smb_proxy_rpc.pb.cc
    smb_proxy_rpc.grpc.pb.cc)
target_link_internal_libraries(apache_util_smb_proxy_rpc_proto
    apache_util_base_error)
target_link_external_libraries(apache_util_smb_proxy_rpc_proto
    grpc
    grpc++
    cares)
