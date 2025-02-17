////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <chrono>
#include "Basics/Common.h"

#include "Basics/system-functions.h"

inline std::string timepointToString(
    std::chrono::system_clock::time_point const& t) {
  time_t tt = std::chrono::system_clock::to_time_t(t);
  struct tm tb;
  size_t const len(21);
  char buffer[len];
  TRI_gmtime(tt, &tb);
  ::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tb);
  return std::string(buffer, len - 1);
}

inline std::string timepointToString(
    std::chrono::system_clock::duration const& d) {
  return timepointToString(std::chrono::system_clock::time_point() + d);
}

inline std::chrono::system_clock::time_point stringToTimepoint(
    std::string_view s) {
  auto const strvtoi = [](std::string_view sv) -> int {
    return static_cast<int>(std::strtol(sv.data(), nullptr, 10));
  };

  if (!s.empty()) {
    try {
      std::tm tt{};
      tt.tm_year = strvtoi(s.substr(0, 4)) - 1900;
      tt.tm_mon = strvtoi(s.substr(5, 2)) - 1;
      tt.tm_mday = strvtoi(s.substr(8, 2));
      tt.tm_hour = strvtoi(s.substr(11, 2));
      tt.tm_min = strvtoi(s.substr(14, 2));
      tt.tm_sec = strvtoi(s.substr(17, 2));
      tt.tm_isdst = 0;
      auto time_c = TRI_timegm(&tt);
      return std::chrono::system_clock::from_time_t(time_c);
    } catch (...) {
    }
  }
  return {};
}
