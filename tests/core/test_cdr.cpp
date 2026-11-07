// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "cdr.hpp"

using rmw_qnx::core::CdrReader;
using rmw_qnx::core::CdrWriter;

TEST(Cdr, the_buffer_starts_with_the_little_endian_encapsulation_header) {
  std::vector<uint8_t> b;
  CdrWriter w(&b);
  EXPECT_EQ(b, (std::vector<uint8_t>{0, 1, 0, 0}));
}

TEST(Cdr, primitives_are_aligned_to_their_size_from_the_body_start) {
  std::vector<uint8_t> b;
  CdrWriter w(&b);
  w.put<uint8_t>(1);
  w.put<uint32_t>(2);
  w.put<uint8_t>(3);
  w.put<double>(4.0);
  // header 4 | u8 at 0 | pad 3 | u32 at 4 | u8 at 8 | pad 7 | f64 at 16
  EXPECT_EQ(b.size(), 4u + 24u);
  EXPECT_EQ(b[4 + 4], 2);
  EXPECT_EQ(b[4 + 8], 3);
}

TEST(Cdr, a_string_carries_its_length_with_the_terminating_nul) {
  std::vector<uint8_t> b;
  CdrWriter w(&b);
  w.put_string("hi", 2);
  EXPECT_EQ(b, (std::vector<uint8_t>{0, 1, 0, 0, 3, 0, 0, 0, 'h', 'i', 0}));
}

TEST(Cdr, a_buffer_without_the_header_is_refused) {
  const uint8_t b[] = {1, 0, 0, 0, 5};
  EXPECT_FALSE(CdrReader(b, sizeof b).ok());
  EXPECT_FALSE(CdrReader(b, 2).ok());
}

TEST(Cdr, a_string_without_its_nul_is_refused) {
  const uint8_t b[] = {0, 1, 0, 0, 2, 0, 0, 0, 'h', 'i'};
  CdrReader r(b, sizeof b);
  std::string s;
  EXPECT_FALSE(r.get_string(&s));
}

TEST(Cdr, a_forged_sequence_length_larger_than_the_buffer_is_refused) {
  std::vector<uint8_t> b;
  CdrWriter w(&b);
  w.put<uint32_t>(1000000);
  w.put<uint32_t>(7);
  CdrReader r(b.data(), b.size());
  uint32_t n = 0;
  EXPECT_FALSE(r.get_length(&n, 4));
}

TEST(Cdr, after_a_failed_read_every_later_read_fails) {
  const uint8_t b[] = {0, 1, 0, 0, 1};
  CdrReader r(b, sizeof b);
  uint32_t v;
  EXPECT_FALSE(r.get(&v));
  uint8_t u;
  EXPECT_FALSE(r.get(&u));
}

namespace {

struct Value {
  uint8_t a;
  int16_t b;
  int64_t c;
  double d;
  std::string s;
  std::vector<int32_t> v;
  bool operator==(const Value &o) const {
    return a == o.a && b == o.b && c == o.c && d == o.d && s == o.s && v == o.v;
  }
};

void write(const Value &x, std::vector<uint8_t> *b) {
  CdrWriter w(b);
  w.put(x.a);
  w.put(x.b);
  w.put(x.c);
  w.put(x.d);
  w.put_string(x.s.data(), x.s.size());
  w.put<uint32_t>(static_cast<uint32_t>(x.v.size()));
  for (int32_t e : x.v) w.put(e);
}

bool read(const std::vector<uint8_t> &b, size_t len, Value *x) {
  CdrReader r(b.data(), len);
  uint32_t n = 0;
  if (!(r.ok() && r.get(&x->a) && r.get(&x->b) && r.get(&x->c) && r.get(&x->d) && r.get_string(&x->s) &&
        r.get_length(&n, 4)))
    return false;
  x->v.resize(n);
  for (auto &e : x->v)
    if (!r.get(&e)) return false;
  return true;
}

Value random_value(std::mt19937_64 &rng) {
  Value x;
  x.a = static_cast<uint8_t>(rng());
  x.b = static_cast<int16_t>(rng());
  x.c = static_cast<int64_t>(rng());
  x.d = static_cast<double>(rng()) / 3.0;
  x.s.assign(rng() % 20, 'x');
  for (auto &ch : x.s) ch = static_cast<char>('a' + rng() % 26);
  x.v.resize(rng() % 10);
  for (auto &e : x.v) e = static_cast<int32_t>(rng());
  return x;
}

}  // namespace

// P5: deserialise(serialise(x)) == x.
TEST(CdrProperty, values_round_trip) {
  std::mt19937_64 rng(42);
  for (int i = 0; i < 2000; i++) {
    const Value x = random_value(rng);
    std::vector<uint8_t> b;
    write(x, &b);
    Value y;
    ASSERT_TRUE(read(b, b.size(), &y));
    ASSERT_EQ(x, y);
  }
}

// P6: every strict truncation of a valid buffer fails to decode, and never reads past the truncated length (the
// reader gets a copy exactly that long, so an overread would show under a memory checker).
TEST(CdrProperty, every_truncation_fails_cleanly) {
  std::mt19937_64 rng(7);
  for (int i = 0; i < 200; i++) {
    const Value x = random_value(rng);
    std::vector<uint8_t> b;
    write(x, &b);
    for (size_t len = 0; len < b.size(); len++) {
      std::vector<uint8_t> cut(b.begin(), b.begin() + static_cast<long>(len));
      Value y;
      ASSERT_FALSE(read(cut, cut.size(), &y)) << "length " << len << " of " << b.size();
    }
  }
}

TEST(Cdr, a_null_buffer_is_refused_whatever_length_it_claims) {
  EXPECT_FALSE(CdrReader(nullptr, 8).ok());
  EXPECT_FALSE(CdrReader(nullptr, 0).ok());
  CdrReader r(nullptr, 8);
  uint32_t n = 0;
  EXPECT_FALSE(r.get(&n));
  EXPECT_EQ(r.remaining(), 0u);
}

// The buffer of a nested sequence whose element size the caller cannot bound: 00 01 00 00 ff ff ff ff.
TEST(Cdr, a_length_of_2_to_the_32_minus_1_is_refused_even_with_no_element_size_given) {
  const uint8_t b[] = {0x00, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
  CdrReader r(b, sizeof b);
  uint32_t n = 0;
  EXPECT_FALSE(r.get_length(&n, 0));
}

// Property: over random hostile buffers and random sequences of reads, the reader never reports more than the
// buffer holds. Every accepted length times its element size fits in what remained, every string fits, remaining()
// only shrinks, and a reader that failed stays failed. The buffer is a heap block of exactly its length, so a
// sanitizer build also catches any read past it.
TEST(Cdr, no_sequence_of_reads_on_hostile_bytes_claims_more_than_the_buffer_holds) {
  std::mt19937 rng(11);
  for (int round = 0; round < 20000; round++) {
    const size_t len = rng() % 64;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[len ? len : 1]);
    for (size_t i = 0; i < len; i++) buf[i] = uint8_t(rng() % 4 == 0 ? 0xff : rng());
    if (len >= 2 && rng() % 4 != 0) {
      buf[0] = 0;
      buf[1] = 1;
    }
    CdrReader r(buf.get(), len);
    size_t before = r.remaining();
    bool failed = !r.ok();
    for (int op = 0; op < 12; op++) {
      bool ok = false;
      switch (rng() % 6) {
        case 0: {
          uint8_t v;
          ok = r.get(&v);
          break;
        }
        case 1: {
          uint32_t v;
          ok = r.get(&v);
          break;
        }
        case 2: {
          double v;
          ok = r.get(&v);
          break;
        }
        case 3: {
          std::string s;
          ok = r.get_string(&s);
          if (ok) {
            EXPECT_LT(s.size(), len);
          }
          break;
        }
        case 4: {
          uint32_t n = 0;
          const size_t elem = rng() % 9;  // 0 means "unknown"
          ok = r.get_length(&n, elem);
          if (ok) {
            EXPECT_LE(uint64_t(n) * (elem ? elem : 1), r.remaining()) << "round " << round;
          }
          break;
        }
        default: {
          uint8_t out[16];
          ok = r.get_bytes(out, rng() % 17);
          break;
        }
      }
      if (failed) {
        EXPECT_FALSE(ok) << "round " << round;
      }
      failed |= !ok;
      EXPECT_LE(r.remaining(), before) << "round " << round;
      before = r.remaining();
    }
  }
}
