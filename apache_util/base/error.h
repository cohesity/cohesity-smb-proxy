// Copyright (C) 2019-2020 Cohesity, Inc.
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
