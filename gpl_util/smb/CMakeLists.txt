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

add_libraries(gpl_util_smb_util util.cc)

add_libraries(smb_proxy
    ops/create_restore_files_op.cc
    ops/create_symlink_op.cc
    ops/delete_entity_op.cc
    ops/fetch_file_handle_op.cc
    ops/finalize_restore_files_op.cc
    ops/rename_entity_op.cc
    ops/verify_share_op.cc
    ops/write_files_data_op.cc
    smb_proxy.cc)
target_link_internal_libraries(smb_proxy
    apache_util_http_server
    apache_util_net
    apache_util_smb_proxy_rpc_proto
    gpl_util_grpclib
    gpl_util_smb_util)
target_link_external_libraries(smb_proxy
    boost54_filesystem
    boost54_system
    boost54_thread
    smbclient)

add_executable(smb_proxy_exec smb_proxy_exec.cc)
# Explicitly link ssl and crypto libraries so that the latest ones are picked
# up at runtime, even when some dependent library is compiled with an older
# version of openssl(which needs to be binary compatible). We need this hack
# because our binaries use RUNPATH. So, the RUNPATH of the dependent library
# takes precedence sometimes depending on the order. The right way to do this
# is by using RPATH on cohesity executables, but that results in segfaults (
# probably due to glibc version mismatch).
target_link_internal_libraries(smb_proxy_exec smb_proxy ssl crypto)
if(CMAKE_CUSTOM_GPERFTOOLS_DISABLE STREQUAL "0")
    target_link_external_libraries(smb_proxy_exec tcmalloc_and_profiler)
endif()
