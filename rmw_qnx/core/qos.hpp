// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// QoS decisions, free of rmw and QNX headers: whether a publisher and a subscription may connect, and the accounting
// behind the requested and offered deadline events (the timer that drives it is a QNX timer delivering a pulse).
#pragma once

#include <cstdint>
#include <string>

namespace rmw_qnx::core {

enum class Reliability { kSystemDefault, kReliable, kBestEffort, kUnknown };
enum class Durability { kSystemDefault, kTransientLocal, kVolatile, kUnknown };
enum class Liveliness { kSystemDefault, kAutomatic, kManualByTopic, kUnknown };
enum class Compat { kOk, kWarning, kError };

struct Qos {
  Reliability reliability = Reliability::kReliable;
  Durability durability = Durability::kVolatile;
  Liveliness liveliness = Liveliness::kAutomatic;
  uint64_t deadline_ns = 0;  // 0: infinite
  uint64_t lease_ns = 0;     // 0: infinite
};

struct CompatResult {
  Compat level = Compat::kOk;
  std::string reason;
};

inline uint64_t finite(uint64_t ns) { return ns == 0 ? UINT64_MAX : ns; }

// The rules of rmw_qos_profile_check_compatible: errors where the subscription asks for more than the publisher
// offers; warnings where a policy is not known yet (system default or unknown), since it may resolve either way.
inline CompatResult check_compatible(const Qos &pub, const Qos &sub) {
  CompatResult r;
  auto err = [&](const char *why) {
    r.level = Compat::kError;
    r.reason += why;
  };
  auto warn = [&](const char *why) {
    if (r.level == Compat::kOk) r.level = Compat::kWarning;
    r.reason += why;
  };
  if (pub.reliability == Reliability::kBestEffort && sub.reliability == Reliability::kReliable)
    err("ERROR: Best effort publisher and reliable subscription;");
  if (pub.durability == Durability::kVolatile && sub.durability == Durability::kTransientLocal)
    err("ERROR: Volatile publisher and transient local subscription;");
  if (finite(pub.deadline_ns) > finite(sub.deadline_ns)) err("ERROR: Subscription has a deadline shorter than publisher's;");
  if (pub.liveliness == Liveliness::kAutomatic && sub.liveliness == Liveliness::kManualByTopic)
    err("ERROR: Publisher's liveliness is automatic and subscription's is manual by topic;");
  if (finite(pub.lease_ns) > finite(sub.lease_ns))
    err("ERROR: Subscription has a liveliness lease duration shorter than publisher's;");
  if (r.level == Compat::kError) return r;
  auto unknown_rel = [](Reliability x) { return x == Reliability::kSystemDefault || x == Reliability::kUnknown; };
  auto unknown_dur = [](Durability x) { return x == Durability::kSystemDefault || x == Durability::kUnknown; };
  auto unknown_liv = [](Liveliness x) { return x == Liveliness::kSystemDefault || x == Liveliness::kUnknown; };
  if ((unknown_rel(pub.reliability) && sub.reliability == Reliability::kReliable) ||
      (pub.reliability == Reliability::kBestEffort && unknown_rel(sub.reliability)))
    warn("WARNING: Reliability may not match;");
  if ((unknown_dur(pub.durability) && sub.durability == Durability::kTransientLocal) ||
      (pub.durability == Durability::kVolatile && unknown_dur(sub.durability)))
    warn("WARNING: Durability may not match;");
  if ((unknown_liv(pub.liveliness) && sub.liveliness == Liveliness::kManualByTopic) ||
      (pub.liveliness == Liveliness::kAutomatic && unknown_liv(sub.liveliness)))
    warn("WARNING: Liveliness may not match;");
  return r;
}

// Deadline accounting for one endpoint. The timer fires every period; a firing with no message since the previous
// one is one missed deadline. take() returns the counts for the event and clears the delta, as rmw's event status
// requires (total_count, total_count_change).
class DeadlineTracker {
 public:
  explicit DeadlineTracker(uint64_t period_ns) : period_(period_ns) {}
  bool enabled() const { return period_ != 0; }
  uint64_t period_ns() const { return period_; }

  void on_message() { seen_ = true; }
  // Returns whether this firing is a miss.
  bool on_timer() {
    if (!enabled()) return false;
    const bool miss = !seen_;
    seen_ = false;
    if (miss) {
      total_++;
      delta_++;
    }
    return miss;
  }
  struct Status {
    uint64_t total, change;
  };
  Status take() {
    Status s{total_, delta_};
    delta_ = 0;
    return s;
  }
  bool pending() const { return delta_ > 0; }

 private:
  uint64_t period_;
  bool seen_ = false;
  uint64_t total_ = 0, delta_ = 0;
};

}  // namespace rmw_qnx::core
