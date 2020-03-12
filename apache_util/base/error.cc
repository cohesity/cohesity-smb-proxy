// Copyright (C) 2017-2020 Cohesity, Inc.
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

#include <glog/logging.h>

#include "apache_util/base/error.h"

using std::string;

namespace cohesity { namespace apache_util {

//-----------------------------------------------------------------------------

namespace {

// Converts errnum into a string.
string ErrnumToString(int errnum) {
  char buf[128];
  int ret = google::posix_strerror_r(errnum, buf, sizeof(buf));

  if ((ret < 0) || (buf[0] == '\0')) {
    return "Error number " + std::to_string(errnum);
  }

  return buf;
}

}  // namespace

//-----------------------------------------------------------------------------

Error::Error(Error::Type type, string error_detail) {
  set_type(type);
  set_error_detail(move(error_detail));
}

//-----------------------------------------------------------------------------

string Error::ToString() const {
  if (Ok()) {
    return ToString(type());
  }
  return "[" + ToString(type())+ "]: " + error_detail();
}

//-----------------------------------------------------------------------------

void Error::CheckOk() const {
  CHECK_EQ(type(), kNoError) << ToString();
}

//-----------------------------------------------------------------------------

// static
const string& Error::ToString(Type err) {
  return ErrorProto::Type_Name(err);
}

//-----------------------------------------------------------------------------

// static
Error Error::ErrorFromErrorNo(int errnum, string error_detail) {
  Error error;

  // No error.
  if (!errnum) {
    return error;
  }

  switch (errnum) {
    case EACCES:
      error.set_type(Error::kPermissionDenied);
      break;

    case ENAMETOOLONG:
      error.set_type(Error::kNameTooLong);
      break;

    case ENOENT:
    case ENOTDIR:
      error.set_type(Error::kNonExistent);
      break;

    case ENOTSUP:
      error.set_type(Error::kNotSupported);
      break;

    case ERANGE:
      error.set_type(Error::kBufferTooSmall);
      break;

    case E2BIG:
      error.set_type(Error::kSizeExceededSystemLimit);
      break;

    case ENODATA:
      error.set_type(Error::kAttrNonExistent);
      break;

    default:
      error.set_type(Error::kInvalid);
      break;
  }

  error.set_error_detail(error_detail + ": " + ErrnumToString(errnum));
  return error;
}

//-----------------------------------------------------------------------------

} }  // namespace cohesity, apache_util
