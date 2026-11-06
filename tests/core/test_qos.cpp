// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <random>

#include "qos.hpp"

using namespace rmw_qnx::core;  // NOLINT

TEST(QosCompat, the_default_pair_is_compatible) { EXPECT_EQ(check_compatible(Qos{}, Qos{}).level, Compat::kOk); }

TEST(QosCompat, a_best_effort_publisher_cannot_feed_a_reliable_subscription) {
  Qos p, s;
  p.reliability = Reliability::kBestEffort;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kError);
}

TEST(QosCompat, a_reliable_publisher_can_feed_a_best_effort_subscription) {
  Qos p, s;
  s.reliability = Reliability::kBestEffort;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kOk);
}

TEST(QosCompat, a_volatile_publisher_cannot_feed_a_transient_local_subscription) {
  Qos p, s;
  s.durability = Durability::kTransientLocal;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kError);
}

TEST(QosCompat, a_publisher_deadline_longer_than_the_subscriptions_is_an_error) {
  Qos p, s;
  p.deadline_ns = 200;
  s.deadline_ns = 100;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kError);
  s.deadline_ns = 200;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kOk);
}

TEST(QosCompat, an_infinite_publisher_deadline_cannot_meet_a_finite_subscription_deadline) {
  Qos p, s;
  s.deadline_ns = 100;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kError);
}

TEST(QosCompat, an_automatic_publisher_cannot_meet_a_manual_by_topic_subscription) {
  Qos p, s;
  s.liveliness = Liveliness::kManualByTopic;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kError);
}

TEST(QosCompat, a_publisher_lease_longer_than_the_subscriptions_is_an_error) {
  Qos p, s;
  p.lease_ns = 2;
  s.lease_ns = 1;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kError);
}

TEST(QosCompat, an_unresolved_reliability_is_a_warning_not_an_error) {
  Qos p, s;
  p.reliability = Reliability::kSystemDefault;
  CompatResult r = check_compatible(p, s);
  EXPECT_EQ(r.level, Compat::kWarning);
  EXPECT_NE(r.reason.find("Reliability"), std::string::npos);
}

TEST(QosCompat, an_error_wins_over_a_warning) {
  Qos p, s;
  p.reliability = Reliability::kBestEffort;
  s.durability = Durability::kSystemDefault;
  EXPECT_EQ(check_compatible(p, s).level, Compat::kError);
}

TEST(Deadline, a_disabled_tracker_never_misses) {
  DeadlineTracker d(0);
  EXPECT_FALSE(d.on_timer());
  EXPECT_EQ(d.take().total, 0u);
}

TEST(Deadline, a_period_with_a_message_is_not_a_miss) {
  DeadlineTracker d(10);
  d.on_message();
  EXPECT_FALSE(d.on_timer());
}

TEST(Deadline, a_period_without_a_message_is_one_miss) {
  DeadlineTracker d(10);
  EXPECT_TRUE(d.on_timer());
  auto s = d.take();
  EXPECT_EQ(s.total, 1u);
  EXPECT_EQ(s.change, 1u);
}

TEST(Deadline, taking_the_status_clears_the_change_but_keeps_the_total) {
  DeadlineTracker d(10);
  d.on_timer();
  d.take();
  d.on_timer();
  auto s = d.take();
  EXPECT_EQ(s.total, 2u);
  EXPECT_EQ(s.change, 1u);
  EXPECT_FALSE(d.pending());
}

// P11: for a random pattern of periods with and without data, the total equals the number of empty periods.
TEST(DeadlineProperty, the_total_counts_exactly_the_empty_periods) {
  std::mt19937 rng(5);
  for (int round = 0; round < 200; round++) {
    DeadlineTracker d(1000);
    uint64_t empty = 0, change = 0, total_taken = 0;
    for (int period = 0; period < 100; period++) {
      const int msgs = int(rng() % 3);
      for (int i = 0; i < msgs; i++) d.on_message();
      empty += msgs == 0;
      d.on_timer();
      if (rng() % 5 == 0) {
        auto s = d.take();
        change += s.change;
        total_taken = s.total;
      }
    }
    auto s = d.take();
    ASSERT_EQ(s.total, empty);
    ASSERT_EQ(change + s.change, empty);
    ASSERT_LE(total_taken, s.total);
  }
}
