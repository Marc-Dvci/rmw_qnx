// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <string>

#include "graph.hpp"

using rmw_qnx::core::Endpoint;
using rmw_qnx::core::EndpointKind;
using rmw_qnx::core::GraphPart;
using rmw_qnx::core::NodeEntry;
using rmw_qnx::core::read_part;
using rmw_qnx::core::write_part;

namespace {
std::string random_text(std::mt19937 &rng) {
  const char alphabet[] = "ab/_%\t\n\r~Z09";
  std::string s;
  const size_t n = rng() % 12;
  for (size_t i = 0; i < n; i++) s += alphabet[rng() % (sizeof alphabet - 1)];
  return s;
}
}  // namespace

// Property: any part written is read back equal, whatever bytes its names hold.
TEST(GraphPart, AnyPartWrittenIsReadBackUnchanged) {
  std::mt19937 rng(3);
  for (int round = 0; round < 2000; round++) {
    GraphPart p;
    p.pid = int64_t(rng());
    p.chid = int64_t(rng() % 100);
    for (int i = int(rng() % 3); i > 0; i--) p.nodes.push_back(NodeEntry{random_text(rng), random_text(rng), "/"});
    for (int i = int(rng() % 4); i > 0; i--) {
      Endpoint e{};
      e.kind = EndpointKind(rng() % 4);
      e.name = random_text(rng);
      e.type = random_text(rng);
      e.node_name = random_text(rng);
      e.node_namespace = random_text(rng);
      for (auto &b : e.gid) b = uint8_t(rng());
      for (auto &q : e.qos) q = int64_t(rng() % 1000);
      p.endpoints.push_back(e);
    }
    GraphPart back;
    ASSERT_TRUE(read_part(write_part(p), &back)) << "round " << round;
    EXPECT_EQ(back.pid, p.pid);
    EXPECT_EQ(back.chid, p.chid);
    ASSERT_EQ(back.nodes.size(), p.nodes.size());
    for (size_t i = 0; i < p.nodes.size(); i++) {
      EXPECT_EQ(back.nodes[i].name, p.nodes[i].name);
      EXPECT_EQ(back.nodes[i].ns, p.nodes[i].ns);
    }
    ASSERT_EQ(back.endpoints.size(), p.endpoints.size());
    for (size_t i = 0; i < p.endpoints.size(); i++) {
      const Endpoint &a = p.endpoints[i], &b = back.endpoints[i];
      EXPECT_EQ(a.kind, b.kind);
      EXPECT_EQ(a.name, b.name);
      EXPECT_EQ(a.type, b.type);
      EXPECT_EQ(a.node_name, b.node_name);
      EXPECT_EQ(a.node_namespace, b.node_namespace);
      EXPECT_EQ(0, std::memcmp(a.gid, b.gid, 16));
      for (int k = 0; k < 5; k++) EXPECT_EQ(a.qos[k], b.qos[k]);
    }
  }
}

TEST(GraphPart, MalformedLinesAreSkippedAndAMissingContextLineIsRefused) {
  GraphPart p;
  EXPECT_FALSE(read_part("N\ttalker\t/\t/\n", &p));
  ASSERT_TRUE(read_part("C\t12\t3\nE\t9\t/x\tt\tn\t/\t00\t1\t1\t1\t1\t1\nN\tbad%Z1\t/\t/\nN\tok\t/\t/\nE\t0\n", &p));
  EXPECT_EQ(p.pid, 12);
  ASSERT_EQ(p.nodes.size(), 1u);
  EXPECT_EQ(p.nodes[0].name, "ok");
  EXPECT_TRUE(p.endpoints.empty());
}

// Property: random bytes never crash the reader and never yield an endpoint with an unknown kind.
TEST(GraphPart, RandomBytesAreReadWithoutCrashingAndYieldOnlyKnownKinds) {
  std::mt19937 rng(5);
  for (int round = 0; round < 5000; round++) {
    std::string t = "C\t1\t1\n";
    const size_t n = rng() % 80;
    for (size_t i = 0; i < n; i++) t += "CNE\t\n0123456789abcdef%/"[rng() % 22];
    GraphPart p;
    read_part(t, &p);
    for (const auto &e : p.endpoints) EXPECT_LE(int(e.kind), 3);
  }
}
