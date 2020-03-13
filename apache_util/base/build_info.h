// Copyright 2018 Cohesity Inc.
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

#ifndef _APACHE_UTIL_BASE_BUILD_INFO_H_
#define _APACHE_UTIL_BASE_BUILD_INFO_H_

static const char* kBuildVersion =
#include "build_version.txt"
    ;

static const char* kBuildLastCommitTime =
#include "build_last_commit_time.txt"
    ;

// Make this a function to satisfy the compiler (-Werror=unused-variable). The
// purpose of putting this string in the executable is to find out the builder
// of a custom binary (by running strings -a <exec_path>).
__attribute__((used)) const char* GitUserEmail() {
  static const char* kGitUserEmail = "exec_builder: "
#include "git_user_email.txt"
      ;
  return kGitUserEmail;
}

#endif  // _APACHE_UTIL_BASE_BUILD_INFO_H_
