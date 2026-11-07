// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// A client's queue of requests not yet sent. Each request keeps the priority of the thread that called
// rmw_send_request. The sender thread is woken by one pulse per request, sent at the caller's priority, and the
// kernel delivers pending pulses highest priority first; so the sender takes the oldest request of the priority the
// pulse names, not simply the oldest request. Otherwise a request queued at 10 and one queued at 20 before the
// sender wakes would swap priorities.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace rmw_qnx::core {

struct Outgoing {
  int64_t seq = 0;
  int prio = 0;
  std::vector<uint8_t> bytes;
};

class SendQueue {
 public:
  void push(Outgoing o) { q_.push_back(std::move(o)); }
  bool empty() const { return q_.empty(); }
  size_t size() const { return q_.size(); }
  // The oldest request queued at `prio`; if none (a pulse whose request is gone), the oldest request of the
  // highest priority queued, so no request is left behind.
  std::optional<Outgoing> take(int prio) {
    if (q_.empty()) return std::nullopt;
    auto pick = q_.end();
    for (auto it = q_.begin(); it != q_.end(); ++it)
      if (it->prio == prio) {
        pick = it;
        break;
      }
    if (pick == q_.end()) {
      pick = q_.begin();
      for (auto it = q_.begin(); it != q_.end(); ++it)
        if (it->prio > pick->prio) pick = it;
    }
    Outgoing o = std::move(*pick);
    q_.erase(pick);
    return o;
  }

 private:
  std::deque<Outgoing> q_;
};

}  // namespace rmw_qnx::core
