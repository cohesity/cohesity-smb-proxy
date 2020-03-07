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
// This file defines various utilities functions

#ifndef _GPL_UTIL_BASE_UTIL_H_
#define _GPL_UTIL_BASE_UTIL_H_

#include <functional>
#include <string>

#include "apache_util/base/basictypes.h"

namespace cohesity { namespace gpl_util {

// Generates a string according to the format specified and return it.
std::string StringPrintf(const char* format, ...)
    __attribute__((format(printf, 1, 2)));

// Converts number of bytes to a readable string.
std::string NumBytesToReadableString(int64 num_bytes);

// Returns the current resident memory in bytes used by this process. If any
// error happens during the process, this function returns 0 and logs the error
// messages to ERROR.
int64 GetCurrentResidentMemory();

// Returns the limit set for the resident memory of this process.
int64 GetResidentMemoryLimit();

// Releases any free memory, checks whether the memory usage of this process
// exceeds its limit, and crashes the process if so.
void CheckMemoryUsage();

// This function forks itself, and the parent process starts self monitoring
// the child process. If the child process dies, the parent process will fork
// another child. Only the child process returns from this function. The
// optional 'monitor_fn' is called from the parent process to perform any
// additional monitoring operations, before checking the child process's pid
// status. If 'monitor_fn" returns false, the parent process will kill and
// respawn a new child process.
void StartSelfMonitoring(std::string program_name,
                         std::function<bool()> monitor_fn = nullptr);

// Reinitializes logging in the child process after fork().
void ReinitializeChild(std::string program_name, int32 parent_pid);
} }  // namespace cohesity, gpl_util

#endif  // _GPL_UTIL_BASE_UTIL_H_
