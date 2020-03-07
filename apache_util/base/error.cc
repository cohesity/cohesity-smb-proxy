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
