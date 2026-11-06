// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// ROS names as file names under /dev/ros2. A fully qualified ROS name such as /robot/joint_states holds '/', which a
// single directory entry cannot. '%' and '/' are percent-encoded; every other byte is kept, so ROS's own characters
// ([A-Za-z0-9_~{}]) read unchanged in `ls`.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace rmw_qnx::core {

inline std::string encode_name(std::string_view ros_name) {
  std::string out;
  out.reserve(ros_name.size() + 8);
  for (char c : ros_name) {
    if (c == '/') {
      out += "%2F";
    } else if (c == '%') {
      out += "%25";
    } else {
      out += c;
    }
  }
  return out;
}

// Returns nothing if the input is not something encode_name could have produced.
inline std::optional<std::string> decode_name(std::string_view file_name) {
  std::string out;
  out.reserve(file_name.size());
  for (size_t i = 0; i < file_name.size(); i++) {
    const char c = file_name[i];
    if (c == '/') return std::nullopt;
    if (c != '%') {
      out += c;
      continue;
    }
    if (i + 2 >= file_name.size()) return std::nullopt;
    const std::string_view esc = file_name.substr(i + 1, 2);
    if (esc == "2F") {
      out += '/';
    } else if (esc == "25") {
      out += '%';
    } else {
      return std::nullopt;
    }
    i += 2;
  }
  return out;
}

}  // namespace rmw_qnx::core
