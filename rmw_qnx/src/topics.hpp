// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// A topic is one shared-memory object, /dev/shmem/rmwqnx.d<domain>.<encoded name>, created by its first endpoint:
// a header (type name, publisher count, subscriber table) followed by the ring of core/ring.hpp. A publisher writes
// one slot and sends each registered subscriber a pulse at the priority that subscriber registered with; the
// subscriber's context receives it on its topic channel. Nothing here blocks a publisher on a subscriber.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "names.hpp"
#include "ring.hpp"

namespace rmw_qnx {

constexpr uint32_t kTopicMagic = 0x54514d52;  // "RMQT"
constexpr int kMaxSubs = 32;
constexpr uint32_t kRemoved = 0x80000000u;
constexpr uint32_t kSlotPayload = 16 * 1024;  // largest message, with its 24-byte prefix
constexpr uint32_t kSlotCount = 64;
constexpr size_t kPrefix = 24;                // publisher gid (16) + publication sequence (8)

struct SubEntry {
  std::atomic<uint32_t> state;  // 0 free, 1 being filled, 2 active
  int32_t pid;
  int32_t chid;
  uint32_t id;
  int32_t prio;
  uint32_t reserved;
};

struct TopicHeader {
  std::atomic<uint32_t> magic;
  // Endpoints using the object (low 31 bits) and, in the top bit, "removed": the last endpoint to leave sets it in
  // the same atomic step that takes the count to zero, then unlinks the object. An endpoint joins only an object
  // without the bit, so none is ever left on an object being removed.
  std::atomic<uint32_t> users;
  char type[192];
  std::atomic<uint32_t> pubs;
  uint32_t reserved2;
  SubEntry subs[kMaxSubs];
  // Endpoints that ever joined this object: matched events count the joins that happened between two looks.
  std::atomic<uint32_t> sub_joins;
  std::atomic<uint32_t> pub_joins;
};

constexpr size_t kRingOffset = (sizeof(TopicHeader) + 63) & ~size_t{63};

inline std::string shm_name(size_t domain, const std::string &topic) {
  return "/rmwqnx.d" + std::to_string(domain) + "." + core::encode_name(topic);
}

class Topic {
 public:
  // Opens or creates the topic, and counts this endpoint as one of its users. Empty error() means success.
  Topic(size_t domain, const std::string &name, const std::string &type) : shm_(shm_name(domain, name)) {
    // An object whose last user is removing it is skipped: open again until the name is free or recreated.
    for (int attempt = 0; attempt < 1000; attempt++) {
      if (open_once(name, type) != kRetry) return;
      usleep(1000);
    }
    err_ = "topic " + name + " stayed in removal";
  }
  ~Topic() {
    for (auto &kv : coids_) ConnectDetach(kv.second);
    if (joined_) leave(h_, shm_);
    if (mem_) munmap(mem_, size_);
  }
  Topic(const Topic &) = delete;
  Topic &operator=(const Topic &) = delete;

  const std::string &error() const { return err_; }
  core::Ring &ring() { return ring_; }
  TopicHeader *header() { return h_; }

  void add_publisher() {
    h_->pubs.fetch_add(1);
    h_->pub_joins.fetch_add(1);
  }
  uint32_t sub_joins() const { return h_->sub_joins.load(); }
  uint32_t pub_joins() const { return h_->pub_joins.load(); }
  void remove_publisher() { h_->pubs.fetch_sub(1); }
  size_t publishers() const { return h_->pubs.load(); }
  size_t subscribers() const {
    size_t n = 0;
    for (const auto &s : h_->subs) n += s.state.load() == 2;
    return n;
  }

  // Registers a subscriber to be pulsed on (pid, chid) with value `id` at priority `prio`. Returns the slot, or -1.
  int add_subscriber(int chid, uint32_t id, int prio) {
    for (int i = 0; i < kMaxSubs; i++) {
      uint32_t expect = 0;
      if (h_->subs[i].state.compare_exchange_strong(expect, 1)) {
        h_->subs[i].pid = getpid();
        h_->subs[i].chid = chid;
        h_->subs[i].id = id;
        h_->subs[i].prio = prio;
        h_->subs[i].state.store(2, std::memory_order_release);
        h_->sub_joins.fetch_add(1);
        return i;
      }
    }
    return -1;
  }
  void remove_subscriber(int slot) {
    if (slot >= 0) h_->subs[slot].state.store(0, std::memory_order_release);
  }

  // Writes one message and pulses every subscriber. Returns false if it does not fit a slot.
  bool publish(const uint8_t *gid, uint64_t pub_seq, const uint8_t *cdr, size_t len, int pulse_code) {
    if (len + kPrefix > kSlotPayload) return false;
    std::lock_guard<std::mutex> l(mu_);
    std::vector<uint8_t> &buf = scratch_;
    buf.resize(kPrefix + len);
    std::memcpy(buf.data(), gid, 16);
    std::memcpy(buf.data() + 16, &pub_seq, 8);
    std::memcpy(buf.data() + kPrefix, cdr, len);
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ring_.write(buf.data(), uint32_t(buf.size()), uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec));
    for (auto &s : h_->subs) {
      if (s.state.load(std::memory_order_acquire) != 2) continue;
      const std::pair<int, int> key{s.pid, s.chid};
      auto it = coids_.find(key);
      if (it == coids_.end()) {
        const int coid = ConnectAttach(0, s.pid, s.chid, _NTO_SIDE_CHANNEL, 0);
        if (coid == -1) {
          if (errno == ESRCH) {
            uint32_t two = 2;
            s.state.compare_exchange_strong(two, 0);  // its process is gone
          }
          continue;
        }
        it = coids_.emplace(key, coid).first;
      }
      // The pulse carries the subscriber's priority, not this publisher's.
      if (MsgSendPulse(it->second, s.prio, pulse_code, int(s.id)) == -1) {
        ConnectDetach(it->second);
        coids_.erase(it);
      }
    }
    return true;
  }

 private:
  enum Outcome { kDone, kRetry };
  Outcome open_once(const std::string &name, const std::string &type) {
    const std::string &n = shm_;
    const size_t size = kRingOffset + core::Ring::bytes_needed(kSlotPayload, kSlotCount);
    int fd = shm_open(n.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
    const bool creator = fd >= 0;
    if (!creator) fd = shm_open(n.c_str(), O_RDWR, 0666);
    if (fd < 0) {
      if (errno == ENOENT) return kRetry;  // removed between the two opens
      err_ = "shm_open " + n + ": " + std::strerror(errno);
      return kDone;
    }
    if (creator && ftruncate(fd, off_t(size)) != 0) {
      err_ = "ftruncate: " + std::string(std::strerror(errno));
      close(fd);
      shm_unlink(n.c_str());
      return kDone;
    }
    if (!creator) {  // the creator may still be sizing it
      struct stat st {};
      for (int i = 0; i < 1000 && (fstat(fd, &st) != 0 || size_t(st.st_size) < size); i++) usleep(1000);
    }
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
      err_ = "mmap: " + std::string(std::strerror(errno));
      return kDone;
    }
    auto *h = static_cast<TopicHeader *>(p);
    if (creator) {
      std::snprintf(h->type, sizeof h->type, "%s", type.c_str());
      core::Ring::format(static_cast<char *>(p) + kRingOffset, kSlotPayload, kSlotCount);
      h->users.store(1, std::memory_order_relaxed);
      h->magic.store(kTopicMagic, std::memory_order_release);
    } else {
      for (int i = 0; i < 1000 && h->magic.load(std::memory_order_acquire) != kTopicMagic; i++) usleep(1000);
      if (h->magic.load(std::memory_order_acquire) != kTopicMagic) {
        munmap(p, size);
        err_ = "topic " + name + " was never initialised";
        return kDone;
      }
      uint32_t u = h->users.load();
      do {
        if (u & kRemoved) {
          munmap(p, size);
          return kRetry;
        }
      } while (!h->users.compare_exchange_weak(u, u + 1));
      if (type != h->type) {
        // Leave again: this endpoint never uses the object. If it was the last user, the name is free now.
        const std::string other = h_type_copy(h);
        const bool was_last = leave(h, n);
        munmap(p, size);
        if (was_last) return kRetry;
        err_ = "topic " + name + " has type " + other + ", not " + type;
        return kDone;
      }
    }
    mem_ = p;
    size_ = size;
    h_ = h;
    joined_ = true;
    ring_ = core::Ring(static_cast<char *>(p) + kRingOffset);
    return kDone;
  }
  static std::string h_type_copy(const TopicHeader *h) { return std::string(h->type, strnlen(h->type, sizeof h->type)); }
  // Takes one user off the object; the last one marks it removed and unlinks it. Returns true for the last.
  static bool leave(TopicHeader *h, const std::string &shm) {
    uint32_t u = h->users.load();
    uint32_t next;
    do {
      next = (u & ~kRemoved) <= 1 ? kRemoved : u - 1;
    } while (!h->users.compare_exchange_weak(u, next));
    if (next == kRemoved) shm_unlink(shm.c_str());
    return next == kRemoved;
  }

  std::string shm_;
  bool joined_ = false;
  std::string err_;
  void *mem_ = nullptr;
  size_t size_ = 0;
  TopicHeader *h_ = nullptr;
  core::Ring ring_{nullptr};
  std::mutex mu_;
  std::map<std::pair<int, int>, int> coids_;
  std::vector<uint8_t> scratch_;
};

}  // namespace rmw_qnx
