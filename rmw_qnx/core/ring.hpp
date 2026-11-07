// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// The topic ring: fixed-size slots in caller-provided memory (a shared-memory object on QNX, a plain buffer in host
// tests). Publishers never block: each write takes the next sequence number and overwrites the slot it maps to.
// Readers keep their own position, never take a message twice, see messages in order, and learn how many they lost
// when a writer lapped them. Each slot carries a sequence lock: the writer makes it odd while writing and sets it to
// 2 * (seq + 1) when done, so a reader can tell a finished slot of the sequence it wants from one being written or
// already overwritten.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rmw_qnx::core {

constexpr uint32_t kRingMagic = 0x524d5751;  // "RMWQ"

struct RingHeader {
  uint32_t magic;
  uint32_t slot_payload;  // bytes of payload per slot
  uint32_t slot_count;
  std::atomic<uint32_t> generation;  // bumped when a subscriber joins or leaves, so publishers know to look again
  std::atomic<uint64_t> next_seq;    // sequence the next write takes
};

struct SlotHeader {
  std::atomic<uint64_t> lock;  // 0 never written; odd: being written; 2 * (seq + 1): holds seq
  uint32_t length;
  uint32_t reserved;
  uint64_t stamp_ns;
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "the ring needs lock-free 64-bit atomics");

enum class TakeStatus { kOk, kEmpty, kBufferTooSmall, kRetry };

struct TakeResult {
  TakeStatus status;
  uint64_t seq = 0;      // sequence of the message taken
  uint32_t length = 0;   // bytes copied
  uint64_t lost = 0;     // messages skipped because a writer lapped this reader
  uint64_t stamp_ns = 0;
};

class Ring {
 public:
  static constexpr size_t slot_stride(uint32_t payload) {
    return (sizeof(SlotHeader) + payload + 7u) & ~size_t{7};
  }
  static constexpr size_t bytes_needed(uint32_t payload, uint32_t count) {
    return sizeof(RingHeader) + slot_stride(payload) * count;
  }

  // Formats `mem` (at least bytes_needed bytes, 8-byte aligned) as an empty ring.
  static Ring format(void *mem, uint32_t payload, uint32_t count) {
    auto *h = static_cast<RingHeader *>(mem);
    h->slot_payload = payload;
    h->slot_count = count;
    h->generation.store(0, std::memory_order_relaxed);
    h->next_seq.store(0, std::memory_order_relaxed);
    Ring r(mem);
    for (uint32_t i = 0; i < count; i++) {
      SlotHeader *s = r.slot(i);
      s->lock.store(0, std::memory_order_relaxed);
      s->length = 0;
    }
    std::atomic_thread_fence(std::memory_order_release);
    h->magic = kRingMagic;
    return r;
  }

  // Attaches to a ring formatted by format(), possibly in another process. valid() says whether it was.
  explicit Ring(void *mem) : h_(static_cast<RingHeader *>(mem)) {}

  bool valid() const { return h_->magic == kRingMagic && h_->slot_count > 0; }
  uint32_t payload() const { return h_->slot_payload; }
  uint32_t count() const { return h_->slot_count; }
  uint64_t head() const { return h_->next_seq.load(std::memory_order_acquire); }
  uint32_t generation() const { return h_->generation.load(std::memory_order_acquire); }
  void bump_generation() { h_->generation.fetch_add(1, std::memory_order_acq_rel); }

  // Writes one message. Returns its sequence, or UINT64_MAX if it does not fit a slot (nothing is written).
  uint64_t write(const void *data, uint32_t length, uint64_t stamp_ns = 0) {
    if (length > h_->slot_payload) return UINT64_MAX;
    const uint64_t seq = h_->next_seq.fetch_add(1, std::memory_order_acq_rel);
    SlotHeader *s = slot(seq % h_->slot_count);
    s->lock.store(2 * seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->length = length;
    s->stamp_ns = stamp_ns;
    std::memcpy(payload_of(s), data, length);
    s->lock.store(2 * (seq + 1), std::memory_order_release);
    return seq;
  }

  // A reader starting now sees only messages written from now on.
  uint64_t reader_start_now() const { return head(); }
  // A reader that also wants the last `depth` messages still in the ring (TRANSIENT_LOCAL).
  uint64_t reader_start_with_history(uint32_t depth) const {
    const uint64_t h = head();
    const uint64_t keep = depth < h_->slot_count ? depth : h_->slot_count;
    return h > keep ? h - keep : 0;
  }

  // Takes the message at *pos (or the oldest still held, if *pos was overwritten), copies it into out, and advances
  // *pos past it. kRetry means a writer was in the slot: call again.
  TakeResult take(uint64_t *pos, void *out, uint32_t capacity) const {
    TakeResult r{TakeStatus::kEmpty};
    const uint64_t h = head();
    if (*pos >= h) return r;
    // The oldest message still held, if a writer lapped this reader.
    const uint64_t want = h - *pos > h_->slot_count ? h - h_->slot_count : *pos;
    const SlotHeader *s = slot(want % h_->slot_count);
    const uint64_t before = s->lock.load(std::memory_order_acquire);
    if (before != 2 * (want + 1)) {
      // Still being written, or overwritten since head() was read: the next call sees the new state.
      r.status = TakeStatus::kRetry;
      return r;
    }
    const uint32_t len = s->length;
    if (len > capacity) {
      r.status = TakeStatus::kBufferTooSmall;
      r.length = len;
      return r;
    }
    std::memcpy(out, payload_of(s), len);
    const uint64_t stamp = s->stamp_ns;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s->lock.load(std::memory_order_relaxed) != before) {
      r.status = TakeStatus::kRetry;  // torn: overwritten while copying
      return r;
    }
    r.lost = want - *pos;
    *pos = want + 1;
    r.status = TakeStatus::kOk;
    r.seq = want;
    r.length = len;
    r.stamp_ns = stamp;
    return r;
  }

 private:
  SlotHeader *slot(uint64_t i) const {
    auto *base = reinterpret_cast<unsigned char *>(h_ + 1);
    return reinterpret_cast<SlotHeader *>(base + slot_stride(h_->slot_payload) * i);
  }
  static unsigned char *payload_of(const SlotHeader *s) {
    return reinterpret_cast<unsigned char *>(const_cast<SlotHeader *>(s) + 1);
  }

  RingHeader *h_;
};

}  // namespace rmw_qnx::core
