// Copyright (C) 2018-2020 Cohesity, Inc.
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
