// Copyright (C) 2013-2020 Cohesity, Inc.
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
// Author: Mohit Aron
//
// An object of type ScopedRunner executes a callback when that object is
// destroyed.

#ifndef _APACHE_UTIL_BASE_SCOPED_RUNNER_H_
#define _APACHE_UTIL_BASE_SCOPED_RUNNER_H_

#include <functional>
#include <memory>

#include "apache_util/base/basictypes.h"

namespace cohesity { namespace apache_util {

class ScopedRunner {
 public:
  typedef std::shared_ptr<ScopedRunner> Ptr;
  typedef std::shared_ptr<const ScopedRunner> PtrConst;

  // The callback to be executed upon destruction is provided by 'cb'.
  //
  // cb is deliberately not made a const reference - callers are encouraged
  // to pass in rvalues (or use std::move to convert lvalues to rvalues).
  explicit ScopedRunner(std::function<void()> cb =
                        std::function<void()>()) : cb_(std::move(cb)) {
  }

  // Move constructor.
  ScopedRunner(ScopedRunner&& other) : cb_(std::move(other.cb_)) { }

  // Move assignment operator.
  ScopedRunner& operator=(ScopedRunner&& other) {
    cb_ = std::move(other.cb_);
    return *this;
  }

  ~ScopedRunner() {
    Run();
  }

  // Run the underlying 'cb' if any.
  void Run() {
    if (cb_) {
      cb_();
      cb_ = nullptr;
    }
  }

  // Reset the underlying callback to 'cb'.
  //
  // cb is deliberately not made a const reference - callers are encouraged
  // to pass in rvalues (or use std::move to convert lvalues to rvalues).
  void Reset(std::function<void()> cb = std::function<void()>()) {
    cb_ = std::move(cb);
  }

  // Returns whether this ScopedRunner has an empty functor.
  bool IsEmpty() const { return !cb_; }

 private:
  std::function<void()> cb_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ScopedRunner);
};

// Catch the bug when name is omitted, e.g., ScopedRunner(cb);
//
// Note,
//  this thing prevents passing (an eventually movable) runner to a function:
//    f(ScopedRunner([x]{ free(x); }));
//
#define ScopedRunner(...) \
  static_assert(0, "ScopedRunner variable missing.")

} }  // namespace cohesity, apache_util

#endif  // _APACHE_UTIL_BASE_SCOPED_RUNNER_H_
