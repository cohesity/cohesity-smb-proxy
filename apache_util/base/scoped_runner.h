// Copyright 2013 Cohesity Inc.
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
