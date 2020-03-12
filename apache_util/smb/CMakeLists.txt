# Copyright (C) 2017-2020 Cohesity, Inc.
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
# 
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
