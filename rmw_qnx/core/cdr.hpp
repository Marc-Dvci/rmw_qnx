// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// CDR (little endian, XCDR1 alignment) writer and bounded reader. Alignment is relative to the start of the body,
// after the 4-byte encapsulation header {0x00, 0x01, 0x00, 0x00} (CDR_LE). Every read checks the remaining length,
// so a truncated or hostile buffer fails instead of reading past its end.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace rmw_qnx::core {

constexpr uint8_t kCdrHeader[4] = {0x00, 0x01, 0x00, 0x00};

class CdrWriter {
 public:
  explicit CdrWriter(std::vector<uint8_t> *out) : out_(out) {
    out_->clear();
    out_->insert(out_->end(), kCdrHeader, kCdrHeader + 4);
  }
  void align(size_t n) {
    while (body() % n) out_->push_back(0);
  }
  template <typename T>
  void put(T v) {
    static_assert(std::is_arithmetic_v<T>, "primitive only");
    align(sizeof(T) > 8 ? 8 : sizeof(T));
    const size_t at = out_->size();
    out_->resize(at + sizeof(T));
    std::memcpy(out_->data() + at, &v, sizeof(T));
  }
  void put_bytes(const void *p, size_t n) {
    const auto *b = static_cast<const uint8_t *>(p);
    out_->insert(out_->end(), b, b + n);
  }
  // A string is its length including the terminating NUL, then the bytes, then the NUL.
  void put_string(const char *s, size_t n) {
    put<uint32_t>(static_cast<uint32_t>(n + 1));
    put_bytes(s, n);
    out_->push_back(0);
  }
  size_t body() const { return out_->size() - 4; }

 private:
  std::vector<uint8_t> *out_;
};

class CdrReader {
 public:
  CdrReader(const uint8_t *data, size_t len) : p_(data), len_(len) {
    ok_ = len >= 4 && data[0] == 0x00 && data[1] == 0x01;
    pos_ = 4;
  }
  bool ok() const { return ok_; }
  size_t remaining() const { return ok_ ? len_ - pos_ : 0; }

  bool align(size_t n) {
    size_t body = pos_ - 4;
    size_t pad = (n - body % n) % n;
    if (!ok_ || pad > len_ - pos_) return ok_ = false;
    pos_ += pad;
    return true;
  }
  template <typename T>
  bool get(T *v) {
    static_assert(std::is_arithmetic_v<T>, "primitive only");
    if (!align(sizeof(T) > 8 ? 8 : sizeof(T)) || sizeof(T) > len_ - pos_) return ok_ = false;
    std::memcpy(v, p_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }
  bool get_bytes(void *out, size_t n) {
    if (!ok_ || n > len_ - pos_) return ok_ = false;
    std::memcpy(out, p_ + pos_, n);
    pos_ += n;
    return true;
  }
  // Reads a sequence or string length and checks that at least `min_elem_size` bytes per element remain, so a
  // forged length cannot make the caller allocate more than the buffer could hold.
  bool get_length(uint32_t *n, size_t min_elem_size) {
    if (!get(n)) return false;
    if (min_elem_size && *n > (len_ - pos_) / min_elem_size) return ok_ = false;
    return true;
  }
  bool get_string(std::string *s) {
    uint32_t n = 0;
    if (!get_length(&n, 1)) return false;
    if (n == 0) {
      s->clear();
      return true;
    }
    if (p_[pos_ + n - 1] != 0) return ok_ = false;  // must end in NUL
    s->assign(reinterpret_cast<const char *>(p_ + pos_), n - 1);
    pos_ += n;
    return true;
  }

 private:
  const uint8_t *p_;
  size_t len_;
  size_t pos_ = 0;
  bool ok_;
};

}  // namespace rmw_qnx::core
