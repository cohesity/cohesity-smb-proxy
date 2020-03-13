// Copyright 2017 Cohesity Inc.
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
// Author: Zheng Cai
//
// This file defines a common error class.

#ifndef _APACHE_UTIL_BASE_ERROR_H_
#define _APACHE_UTIL_BASE_ERROR_H_

#include <algorithm>
#include <string>

#include "apache_util/base/basictypes.h"
#include "apache_util/base/error.pb.h"

namespace cohesity { namespace apache_util {

class Error : public ErrorProto {
 public:
  Error() = default;

  Error(const Error& error) { CopyFrom(error); }  // NOLINT
  Error(const ErrorProto& error) { CopyFrom(error); }  // NOLINT

  explicit Error(Error::Type type, std::string error_detail = "");

  // Move constructor.
  explicit Error(ErrorProto&& error) {
    // This isn't completely faithful, but cheaper than testing has_type.
    set_type(error.type());
    if (error.has_error_detail()) {
      std::swap(*mutable_error_detail(), *error.mutable_error_detail());
    }
    error.Clear();
  }

  ~Error() = default;

  // Returns true if there are no errors, false otherwise.
  bool Ok() const { return type() == kNoError; }
  void CheckOk() const;

  // Returns a string representation of this object.
  std::string ToString() const;

  // Serializes self to 'error'.
  void ToProto(ErrorProto* error) const {
    error->Clear();
    if (has_type()) {
      error->set_type(type());
    }
    if (has_error_detail()) {
      error->set_error_detail(error_detail());
    }
  }

  // Provides a string representation of the error err.
  static const std::string& ToString(Type err);

  // Returns an error based on posix errno and error detail.
  static Error ErrorFromErrorNo(int errnum, std::string error_detail);
};

// Emit 'err' to the output stream 'os'.
inline std::ostream& operator<<(std::ostream& os, Error::Type err) {
  return os << Error::ToString(err);
}

// Emit 'err' to the output stream 'os'.
inline std::ostream& operator<<(std::ostream& os, const Error& err) {
  return os << err.ToString();
}

} }  // namespace cohesity, apache_util

#endif  // _APACHE_UTIL_BASE_ERROR_H_
