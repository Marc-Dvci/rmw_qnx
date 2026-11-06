// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// Serialises ROS messages to CDR from the C++ introspection type support, with core/cdr.hpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cdr.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

namespace rmw_qnx {

namespace its = rosidl_typesupport_introspection_cpp;

inline size_t prim_size(uint8_t t) {
  switch (t) {
    case its::ROS_TYPE_FLOAT: return 4;
    case its::ROS_TYPE_DOUBLE: return 8;
    case its::ROS_TYPE_CHAR:
    case its::ROS_TYPE_BOOLEAN:
    case its::ROS_TYPE_OCTET:
    case its::ROS_TYPE_UINT8:
    case its::ROS_TYPE_INT8: return 1;
    case its::ROS_TYPE_WCHAR:
    case its::ROS_TYPE_UINT16:
    case its::ROS_TYPE_INT16: return 2;
    case its::ROS_TYPE_UINT32:
    case its::ROS_TYPE_INT32: return 4;
    case its::ROS_TYPE_UINT64:
    case its::ROS_TYPE_INT64: return 8;
    default: return 0;  // strings, messages, long double
  }
}

inline void put_prim(core::CdrWriter &w, uint8_t t, const void *p) {
  switch (prim_size(t)) {
    case 1: w.put(*static_cast<const uint8_t *>(p)); break;
    case 2: w.put(*static_cast<const uint16_t *>(p)); break;
    case 4: w.put(*static_cast<const uint32_t *>(p)); break;
    case 8: w.put(*static_cast<const uint64_t *>(p)); break;
    default: break;
  }
}

inline bool get_prim(core::CdrReader &r, uint8_t t, void *p) {
  switch (prim_size(t)) {
    case 1: return r.get(static_cast<uint8_t *>(p));
    case 2: return r.get(static_cast<uint16_t *>(p));
    case 4: return r.get(static_cast<uint32_t *>(p));
    case 8: return r.get(static_cast<uint64_t *>(p));
    default: return false;
  }
}

inline bool supported(uint8_t t) {
  return prim_size(t) != 0 || t == its::ROS_TYPE_STRING || t == its::ROS_TYPE_MESSAGE;
}

inline bool serialize_cpp(const its::MessageMembers *mm, const void *msg, core::CdrWriter &w) {
  const auto *base = static_cast<const uint8_t *>(msg);
  for (uint32_t i = 0; i < mm->member_count_; i++) {
    const its::MessageMember &m = mm->members_[i];
    if (!supported(m.type_id_)) return false;
    const void *field = base + m.offset_;
    const auto *sub = m.type_id_ == its::ROS_TYPE_MESSAGE
                          ? static_cast<const its::MessageMembers *>(m.members_->data)
                          : nullptr;
    if (!m.is_array_) {
      if (sub) {
        if (!serialize_cpp(sub, field, w)) return false;
      } else if (m.type_id_ == its::ROS_TYPE_STRING) {
        const auto *s = static_cast<const std::string *>(field);
        w.put_string(s->data(), s->size());
      } else {
        put_prim(w, m.type_id_, field);
      }
      continue;
    }
    const size_t n = m.size_function(field);
    if (m.array_size_ == 0 || m.is_upper_bound_) w.put<uint32_t>(static_cast<uint32_t>(n));
    for (size_t k = 0; k < n; k++) {
      if (sub) {
        if (!serialize_cpp(sub, m.get_const_function(field, k), w)) return false;
      } else if (m.type_id_ == its::ROS_TYPE_STRING) {
        const auto *s = static_cast<const std::string *>(m.get_const_function(field, k));
        w.put_string(s->data(), s->size());
      } else {
        uint64_t tmp[2] = {0, 0};
        m.fetch_function(field, k, tmp);
        put_prim(w, m.type_id_, tmp);
      }
    }
  }
  return true;
}

inline bool deserialize_cpp(const its::MessageMembers *mm, void *msg, core::CdrReader &r) {
  auto *base = static_cast<uint8_t *>(msg);
  for (uint32_t i = 0; i < mm->member_count_; i++) {
    const its::MessageMember &m = mm->members_[i];
    if (!supported(m.type_id_)) return false;
    void *field = base + m.offset_;
    const auto *sub = m.type_id_ == its::ROS_TYPE_MESSAGE
                          ? static_cast<const its::MessageMembers *>(m.members_->data)
                          : nullptr;
    if (!m.is_array_) {
      if (sub) {
        if (!deserialize_cpp(sub, field, r)) return false;
      } else if (m.type_id_ == its::ROS_TYPE_STRING) {
        if (!r.get_string(static_cast<std::string *>(field))) return false;
      } else if (!get_prim(r, m.type_id_, field)) {
        return false;
      }
      continue;
    }
    size_t n = m.array_size_;
    if (m.array_size_ == 0 || m.is_upper_bound_) {
      uint32_t len = 0;
      const size_t min_elem = prim_size(m.type_id_) ? prim_size(m.type_id_) : (sub ? 0 : 4);
      if (!r.get_length(&len, min_elem)) return false;
      if (m.is_upper_bound_ && len > m.array_size_) return false;
      n = len;
      m.resize_function(field, n);
    }
    for (size_t k = 0; k < n; k++) {
      if (sub) {
        if (!deserialize_cpp(sub, m.get_function(field, k), r)) return false;
      } else if (m.type_id_ == its::ROS_TYPE_STRING) {
        if (!r.get_string(static_cast<std::string *>(m.get_function(field, k)))) return false;
      } else {
        uint64_t tmp[2] = {0, 0};
        if (!get_prim(r, m.type_id_, tmp)) return false;
        m.assign_function(field, k, tmp);
      }
    }
  }
  return true;
}

}  // namespace rmw_qnx
