// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <random>
#include <thread>
#include <vector>

#include "ring.hpp"

using rmw_qnx::core::Ring;
using rmw_qnx::core::TakeResult;
using rmw_qnx::core::TakeStatus;

namespace {

struct Mem {
  explicit Mem(uint32_t payload, uint32_t count) : buf(Ring::bytes_needed(payload, count) / 8 + 1) {}
  void *p() { return buf.data(); }
  std::vector<uint64_t> buf;
};

uint64_t write_u64(Ring &r, uint64_t v) { return r.write(&v, sizeof v); }

TakeResult take_u64(const Ring &r, uint64_t *pos, uint64_t *v) {
  for (;;) {
    TakeResult t = r.take(pos, v, sizeof *v);
    if (t.status != TakeStatus::kRetry) return t;
  }
}

}  // namespace

TEST(Ring, an_unformatted_buffer_is_not_a_valid_ring) {
  Mem m(16, 4);
  EXPECT_FALSE(Ring(m.p()).valid());
  Ring::format(m.p(), 16, 4);
  EXPECT_TRUE(Ring(m.p()).valid());
}

TEST(Ring, a_new_reader_sees_nothing_until_a_write) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  uint64_t pos = r.reader_start_now(), v = 0;
  EXPECT_EQ(r.take(&pos, &v, sizeof v).status, TakeStatus::kEmpty);
}

TEST(Ring, a_reader_takes_each_message_once_in_order) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  uint64_t pos = r.reader_start_now(), v = 0;
  write_u64(r, 10);
  write_u64(r, 11);
  ASSERT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kOk);
  EXPECT_EQ(v, 10u);
  ASSERT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kOk);
  EXPECT_EQ(v, 11u);
  EXPECT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kEmpty);
}

TEST(Ring, a_reader_that_joins_late_misses_earlier_messages) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  write_u64(r, 1);
  uint64_t pos = r.reader_start_now(), v = 0;
  EXPECT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kEmpty);
}

TEST(Ring, a_transient_local_reader_gets_the_last_depth_messages) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  for (uint64_t i = 0; i < 6; i++) write_u64(r, i);
  uint64_t pos = r.reader_start_with_history(2), v = 0;
  ASSERT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kOk);
  EXPECT_EQ(v, 4u);
  ASSERT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kOk);
  EXPECT_EQ(v, 5u);
}

TEST(Ring, history_deeper_than_the_ring_is_capped_at_the_ring) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  for (uint64_t i = 0; i < 10; i++) write_u64(r, i);
  uint64_t pos = r.reader_start_with_history(100), v = 0;
  ASSERT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kOk);
  EXPECT_EQ(v, 6u);
}

TEST(Ring, a_lapped_reader_skips_to_the_oldest_held_and_counts_what_it_lost) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  uint64_t pos = r.reader_start_now(), v = 0;
  for (uint64_t i = 0; i < 7; i++) write_u64(r, i);
  TakeResult t = take_u64(r, &pos, &v);
  ASSERT_EQ(t.status, TakeStatus::kOk);
  EXPECT_EQ(v, 3u);
  EXPECT_EQ(t.lost, 3u);
}

TEST(Ring, a_message_larger_than_a_slot_is_refused_and_nothing_is_written) {
  Mem m(8, 4);
  Ring r = Ring::format(m.p(), 8, 4);
  char big[9] = {};
  EXPECT_EQ(r.write(big, sizeof big), UINT64_MAX);
  EXPECT_EQ(r.head(), 0u);
}

TEST(Ring, a_slot_exactly_full_is_accepted) {
  Mem m(8, 4);
  Ring r = Ring::format(m.p(), 8, 4);
  uint64_t v = 7, pos = 0;
  EXPECT_EQ(r.write(&v, 8), 0u);
  EXPECT_EQ(take_u64(r, &pos, &v).status, TakeStatus::kOk);
}

TEST(Ring, a_buffer_too_small_reports_the_length_and_keeps_the_position) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  uint64_t pos = 0;
  char data[12] = {};
  r.write(data, sizeof data);
  char small[4];
  TakeResult t = r.take(&pos, small, sizeof small);
  EXPECT_EQ(t.status, TakeStatus::kBufferTooSmall);
  EXPECT_EQ(t.length, 12u);
  EXPECT_EQ(pos, 0u);
}

TEST(Ring, the_timestamp_travels_with_the_message) {
  Mem m(16, 4);
  Ring r = Ring::format(m.p(), 16, 4);
  uint64_t v = 1, pos = 0;
  r.write(&v, sizeof v, 123456789);
  EXPECT_EQ(take_u64(r, &pos, &v).stamp_ns, 123456789u);
}

// P3: against a model (a deque of the last `count` messages), for random interleavings of writes and takes by
// several readers, each reader gets exactly the messages written since its position, in order, once each, or
// is told how many it lost.
TEST(RingProperty, readers_match_a_model_for_random_interleavings) {
  for (uint32_t seed = 1; seed <= 200; seed++) {
    std::mt19937 rng(seed);
    const uint32_t count = 1 + rng() % 8;
    Mem m(8, count);
    Ring r = Ring::format(m.p(), 8, count);
    std::vector<uint64_t> written;  // value i is written as sequence i
    struct Reader {
      uint64_t pos, expect;
    };
    std::vector<Reader> readers;
    for (int i = 0; i < 3; i++) readers.push_back({r.reader_start_now(), 0});
    for (int step = 0; step < 300; step++) {
      if (rng() % 2) {
        const uint64_t v = written.size();
        ASSERT_EQ(write_u64(r, v), v);
        written.push_back(v);
        continue;
      }
      Reader &rd = readers[rng() % readers.size()];
      uint64_t v = 0;
      const uint64_t head = written.size();
      TakeResult t = take_u64(r, &rd.pos, &v);
      if (rd.expect >= head) {
        ASSERT_EQ(t.status, TakeStatus::kEmpty);
        continue;
      }
      const uint64_t oldest = head > count ? head - count : 0;
      const uint64_t want = rd.expect < oldest ? oldest : rd.expect;
      ASSERT_EQ(t.status, TakeStatus::kOk) << "seed " << seed;
      ASSERT_EQ(v, want) << "seed " << seed;
      ASSERT_EQ(t.lost, want - rd.expect) << "seed " << seed;
      rd.expect = want + 1;
    }
  }
}

// P4: a writer thread and a reader thread race; every message the reader accepts is whole (all its words carry the
// same sequence), and sequences only increase.
TEST(RingProperty, a_racing_reader_never_returns_a_torn_message) {
  constexpr uint32_t kWords = 32, kCount = 4;
  Mem m(kWords * 8, kCount);
  Ring r = Ring::format(m.p(), kWords * 8, kCount);
  std::atomic<bool> done{false};
  std::thread writer([&] {
    uint64_t msg[kWords];
    for (uint64_t s = 0; s < 200000; s++) {
      for (auto &w : msg) w = s;
      r.write(msg, sizeof msg);
    }
    done = true;
  });
  uint64_t pos = 0, last = 0, got = 0, msg[kWords];
  bool first = true;
  while (!done || pos < r.head()) {
    TakeResult t = r.take(&pos, msg, sizeof msg);
    if (t.status != TakeStatus::kOk) continue;
    for (auto w : msg) ASSERT_EQ(w, msg[0]) << "torn message at sequence " << t.seq;
    ASSERT_EQ(msg[0], t.seq);
    if (!first) {
      ASSERT_GT(t.seq, last);
    }
    first = false;
    last = t.seq;
    got++;
  }
  writer.join();
  EXPECT_GT(got, 0u);
}
