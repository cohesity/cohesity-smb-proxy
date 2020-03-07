# Copyright 2018 Cohesity Inc.
#
# Author: Zheng Cai
#
# Attention: The library in this file should only be used by internal
# code. Remove this file when open sourcing this package.

proto_generate_grpc_cohesity_cpp_only(smb_proxy_rpc.proto)
add_libraries(apache_util_smb_proxy_rpc_proto_client
    smb_proxy_rpc_client.cohesity.pb.cc)
target_link_internal_libraries(apache_util_smb_proxy_rpc_proto_client
    apache_util_smb_proxy_rpc_proto
    open_util_net_protorpc
    util_net_grpclib_client)

add_libraries(apache_smb_proxy_rpc_server smb_proxy_rpc_server.cohesity.pb.cc)
target_link_internal_libraries(apache_smb_proxy_rpc_server
    apache_util_smb_proxy_rpc_proto
    open_util_net_protorpc
    util_net_grpclib_server)
