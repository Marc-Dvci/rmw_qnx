// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "sendq.hpp"

using rmw_qnx::core::Outgoing;
using rmw_qnx::core::SendQueue;

TEST(SendQueue, AHigherPriorityPulseTakesTheHigherPriorityRequestEvenIfItWasQueuedLater) {
  SendQueue q;
  q.push(Outgoing{1, 10, {}});
  q.push(Outgoing{2, 20, {}});
  // The kernel delivers the priority-20 pulse first.
  auto first = q.take(20);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->seq, 2);
  EXPECT_EQ(first->prio, 20);
  auto second = q.take(10);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->seq, 1);
  EXPECT_TRUE(q.empty());
}

TEST(SendQueue, RequestsOfOnePriorityLeaveInTheOrderTheyCame) {
  SendQueue q;
  for (int64_t s = 1; s <= 5; s++) q.push(Outgoing{s, 15, {}});
  for (int64_t s = 1; s <= 5; s++) EXPECT_EQ(q.take(15)->seq, s);
}

TEST(SendQueue, APulseWithNoRequestOfItsPriorityTakesTheHighestQueuedSoNothingIsLeftBehind) {
  SendQueue q;
  q.push(Outgoing{1, 10, {}});
  q.push(Outgoing{2, 12, {}});
  EXPECT_EQ(q.take(30)->seq, 2);
  EXPECT_EQ(q.take(30)->seq, 1);
  EXPECT_FALSE(q.take(30));
}

// Property: requests queued at random priorities, with their pulses delivered the way the kernel does (highest
// priority first, first-in first-out within a priority), are each taken by the pulse of their own priority, and
// within a priority in the order they were queued.
TEST(SendQueue, EveryRequestIsTakenByAPulseOfItsOwnPriorityInKernelDeliveryOrder) {
  std::mt19937 rng(7);
  for (int round = 0; round < 500; round++) {
    SendQueue q;
    struct Pulse {
      int prio;
      int64_t order;
    };
    std::vector<Pulse> pending;
    const int n = 1 + int(rng() % 20);
    int64_t seq = 0;
    std::vector<int64_t> taken_seq;
    for (int i = 0; i < n; i++) {
      const int prio = 1 + int(rng() % 4) * 5;
      q.push(Outgoing{++seq, prio, {}});
      pending.push_back({prio, seq});
      // Sometimes the sender wakes between requests and takes the highest pending pulse.
      if (rng() % 3 == 0) {
        auto it = std::max_element(pending.begin(), pending.end(), [](const Pulse &a, const Pulse &b) {
          return a.prio < b.prio || (a.prio == b.prio && a.order > b.order);
        });
        auto o = q.take(it->prio);
        ASSERT_TRUE(o);
        EXPECT_EQ(o->prio, it->prio) << "round " << round;
        taken_seq.push_back(o->seq);
        pending.erase(it);
      }
    }
    while (!pending.empty()) {
      auto it = std::max_element(pending.begin(), pending.end(), [](const Pulse &a, const Pulse &b) {
        return a.prio < b.prio || (a.prio == b.prio && a.order > b.order);
      });
      auto o = q.take(it->prio);
      ASSERT_TRUE(o);
      EXPECT_EQ(o->prio, it->prio) << "round " << round;
      EXPECT_EQ(o->seq, it->order) << "round " << round;
      pending.erase(it);
    }
    EXPECT_TRUE(q.empty());
  }
}
