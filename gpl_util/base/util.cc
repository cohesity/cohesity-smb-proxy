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

#include <cstdarg>
#include <fcntl.h>
#include <glog/logging.h>

#ifndef GPERFTOOLS_DISABLE
#include <gperftools/malloc_extension.h>
#include <gperftools/malloc_hook.h>
#include <gperftools/profiler.h>
#endif  // GPERFTOOLS_DISABLE

#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "apache_util/base/error.h"
#include "gpl_util/base/util.h"

using cohesity::apache_util::Error;
using std::function;
using std::string;

DEFINE_int64(gpl_util_memory_usage_additional_allowance_MB, 0,
             "Add an extra allowance to the memory limit. This flag can be "
             "used instead of manually modifying the memory limit settings "
             "and restarting the service.");

DEFINE_int32(gpl_util_self_monitoring_sleep_secs, 10,
             "For how many seconds the parent process should sleep between "
             "its self-monitoring checks.");

DEFINE_bool(gpl_util_self_monitoring_enabled, true,
            "Whether the self-monitoring should be enabled.");

namespace cohesity { namespace gpl_util {

//-----------------------------------------------------------------------------

string StringPrintf(const char* format, ...) {
  va_list argptr;

  va_start(argptr, format);
  int rv = vsnprintf(nullptr, 0, format, argptr);
  va_end(argptr);

  std::string output(rv + 1, '\0');  // +1 for '\0'

  // There will be no need for const_cast() in C++17.
  va_start(argptr, format);
  CHECK_EQ(vsnprintf(const_cast<char*>(output.data()), rv + 1, format, argptr),
           rv) << format;
  va_end(argptr);

  // The string has an extra character now (the '\0' at the end). Remove it.
  output.pop_back();

  return output;
}

//-----------------------------------------------------------------------------

string NumBytesToReadableString(int64 usage) {

  const double kNumBytesInKB = 1024.0;
  const double kNumBytesInMB = 1024.0 * kNumBytesInKB;
  const double kNumBytesInGB = 1024.0 * kNumBytesInMB;
  const double kNumBytesInTB = 1024.0 * kNumBytesInGB;

  if (std::abs(usage) >= kNumBytesInTB) {
    return StringPrintf("%.1f TB", usage / kNumBytesInTB);
  }

  if (std::abs(usage) >= kNumBytesInGB) {
    return StringPrintf("%.1f GB", usage / kNumBytesInGB);
  }

  if (std::abs(usage) >= kNumBytesInMB) {
    return StringPrintf("%.1f MB", usage / kNumBytesInMB);
  }

  if (std::abs(usage) >= kNumBytesInKB) {
    return StringPrintf("%.1f KB", usage / kNumBytesInKB);
  }

  return StringPrintf("%" PRId64 "B", usage);
}

//-----------------------------------------------------------------------------

int64 GetCurrentResidentMemory() {
  // Read the process's proc/[pid]/statm file for resident memory usage. The
  // format of this file is (from proc's man page):
  //
  // Provides information about memory usage, measured in pages.
  // The columns are:
  //
  // size       (1) total program size
  //            (same as VmSize in /proc/[pid]/status)
  // resident   (2) resident set size
  //            (same as VmRSS in /proc/[pid]/status)
  // share      (3) shared pages (i.e., backed by a file)
  // text       (4) text (code)
  // lib        (5) library (unused in Linux 2.6)
  // data       (6) data + stack
  // dt         (7) dirty pages (unused in Linux 2.6)
  //
  // Since we only need the resident size, we only need to read the second
  // number.
  const string file_name = "/proc/" + std::to_string(getpid()) + "/statm";
  auto* statm_file = fopen(file_name.c_str(), "r");
  if (!statm_file) {
    LOG(ERROR) << "Failed to open " << file_name << " with errno " << errno;
    return 0;
  }

  int64 rss_num_pages = 0;

  auto ret = fscanf(statm_file, "%*s%ld", &rss_num_pages);
  fclose(statm_file);

  if (ret != 1) {
    LOG(ERROR) << "Failed to read " << file_name << " with errno " << errno;
    return 0;
  }

  return rss_num_pages * sysconf(_SC_PAGESIZE);
}

//-----------------------------------------------------------------------------

int64 GetResidentMemoryLimit() {
  struct rlimit rl = {0};
  CHECK(!getrlimit(RLIMIT_RSS, &rl));
  return rl.rlim_cur;
}

//-----------------------------------------------------------------------------

void CheckMemoryUsage() {
  auto current_resident_memory = GetCurrentResidentMemory();
  const auto resident_memory_limit = GetResidentMemoryLimit();
  const auto extra_allowance_bytes =
      FLAGS_gpl_util_memory_usage_additional_allowance_MB * 1024 * 1024;

#ifndef GPERFTOOLS_DISABLE
  // Try releasing free memory before performing memory checks.
  const auto percent = (100.0 * current_resident_memory /
                        (resident_memory_limit + extra_allowance_bytes));
  LOG(INFO) << StringPrintf(
      "Releasing free memory to system currently using "
      "%s at %f%% of total limit)",
      NumBytesToReadableString(current_resident_memory).c_str(),
      percent);
  MallocExtension::instance()->ReleaseFreeMemory();
#endif  // GPERFTOOLS_DISABLE

  current_resident_memory = GetCurrentResidentMemory();
  if (current_resident_memory >
      resident_memory_limit + extra_allowance_bytes) {
    string malloc_stats;
    string heap_sample;
#ifndef GPERFTOOLS_DISABLE
    char stats_buf[4096];
    MallocExtension::instance()->GetStats(stats_buf, sizeof(stats_buf));
    malloc_stats = stats_buf;
    MallocExtension::instance()->GetHeapSample(&heap_sample);
#else
    malloc_stats = "undef GPERFTOOLS_DISABLE to get malloc stats.";
    heap_sample = "undef GPERFTOOLS_DISABLE to get heap usage.";
#endif

    LOG(FATAL) << "Memory usage: " << current_resident_memory
               << " exceeds limit: " << resident_memory_limit
               << " extra allowance: " << extra_allowance_bytes
               << " malloc stats:\n" << malloc_stats
               << " heap:\n" << heap_sample;
  }
}

//-----------------------------------------------------------------------------

void StartSelfMonitoring(string program_name, function<bool()> monitor_fn) {
  if (!FLAGS_gpl_util_self_monitoring_enabled) {
    return;
  }

  while (true) {
    const auto parent_pid = getpid();
    PCHECK(parent_pid > 0);
    const auto pid = fork();
    PCHECK(pid >= 0);
    if (!pid) {
      ReinitializeChild(move(program_name), parent_pid);
      return;
    }

    // Check the status of the child process periodically, and if it dies,
    // restart a new child.
    bool should_break = false;
    while (true) {
      sleep(FLAGS_gpl_util_self_monitoring_sleep_secs);

      if (monitor_fn && !monitor_fn()) {
        LOG(ERROR) << "Child is not behaving properly";
        auto ret = kill(pid, SIGABRT);
        if (ret == 0) {
          // Break the status checking loop to spawn a new child.
          should_break = true;
        } else {
          auto error = Error::ErrorFromErrorNo(
              errno, "Failed to kill child process " + std::to_string(pid));
          LOG(ERROR) << error.error_detail();
        }
      }

      // Wait for the child process state change.
      pid_t ret = -1;
      int status;
      do {
        ret = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
      } while (ret == -1 && errno == EINTR);

      // If the state of the child is not changed when WNOHANG is specified,
      // waitpid returns 0.
      if (!ret) {
        continue;
      }

      if (WIFCONTINUED(status)) {
        LOG(INFO) << "Child with pid " << pid << " was continued.";
        continue;
      }

      if (WIFSTOPPED(status)) {
        LOG(INFO)
            << "Child with pid " << pid << " was found to be stopped "
            << "by signal " << WSTOPSIG(status)
            << " not spawning another child and waiting until the current "
            << "child is continued.";
        continue;
      } else if (WIFSIGNALED(status)) {
        LOG(INFO) << "Child with pid " << pid
                  << " was killed by signal: " << WTERMSIG(status);
        break;
      } else if (WIFEXITED(status)) {
        LOG(INFO) << "Child with pid " << pid << " exited with status "
                  << WEXITSTATUS(status);
        break;
      } else {
        // This should never happen as our if conditions covered all the
        // situations documented in the man page for waitpid. But keeping this
        // print statement for completeness sake. This could also have been a
        // LOG(FATAL), but LOG(FATAL) will cause very hard to debug problems if
        // the self monitoring parent dies.
        LOG(INFO) << "Unknown condition for child " << pid
                  << ". Going ahead and spwaning another child anyway.";
        break;
      }

      if (should_break) {
        LOG(ERROR) << "Rrestarting child now";
        break;
      }
    }
  }
}

//-----------------------------------------------------------------------------

void ReinitializeChild(string program_name, int32 parent_pid) {
  // Re-initialize logging to make sure we don't reuse any log files of the
  // parent process.
  google::ShutdownGoogleLogging();

  // Reset symlinks for the child process.
  google::SetLogSymlink(google::GLOG_INFO, program_name.c_str());
  google::SetLogSymlink(google::GLOG_WARNING, program_name.c_str());
  google::SetLogSymlink(google::GLOG_ERROR, program_name.c_str());
  google::SetLogSymlink(google::GLOG_FATAL, program_name.c_str());

  google::InitGoogleLogging(program_name.c_str());
  google::InstallFailureSignalHandler();

  // Let the child die when the parent is killed. Otherwise, we will end up
  // with an orphan process. Use SIGKILL as that cannot be masked by the
  // child.
  PCHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL));

  const auto child_pid = getpid();

  DCHECK_NE(child_pid, parent_pid);
  LOG(INFO) << "Current process " << child_pid << " was forked by parent "
            << parent_pid;

#ifndef GPERFTOOLS_DISABLE
  ProfilerRegisterThread();
#endif  // GPERFTOOLS_DISABLE
}

//-----------------------------------------------------------------------------

} }  // namespace cohesity, gpl_util
