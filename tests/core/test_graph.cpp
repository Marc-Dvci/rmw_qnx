// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "graph.hpp"

using rmw_qnx::core::Endpoint;
using rmw_qnx::core::EndpointKind;
using rmw_qnx::core::Graph;
using rmw_qnx::core::NodeEntry;

namespace {

Endpoint ep(EndpointKind k, std::string name, std::string type = "std_msgs/msg/String", std::string node = "n",
            std::string ns = "/") {
  return Endpoint{k, std::move(name), std::move(type), std::move(node), std::move(ns), {}};
}

}  // namespace

TEST(Graph, an_empty_graph_counts_nothing) {
  Graph g;
  EXPECT_EQ(g.count(EndpointKind::kPublisher, "/chatter"), 0u);
  EXPECT_TRUE(g.nodes().empty());
}

TEST(Graph, publishers_and_subscriptions_are_counted_apart) {
  Graph g;
  g.add_endpoint(1, ep(EndpointKind::kPublisher, "/chatter"));
  g.add_endpoint(1, ep(EndpointKind::kSubscription, "/chatter"));
  g.add_endpoint(2, ep(EndpointKind::kSubscription, "/chatter"));
  EXPECT_EQ(g.count(EndpointKind::kPublisher, "/chatter"), 1u);
  EXPECT_EQ(g.count(EndpointKind::kSubscription, "/chatter"), 2u);
}

TEST(Graph, an_owner_cannot_remove_another_owners_entry) {
  Graph g;
  const auto id = g.add_endpoint(1, ep(EndpointKind::kService, "/add"));
  EXPECT_FALSE(g.remove(2, id));
  EXPECT_EQ(g.count(EndpointKind::kService, "/add"), 1u);
  EXPECT_TRUE(g.remove(1, id));
  EXPECT_EQ(g.count(EndpointKind::kService, "/add"), 0u);
}

TEST(Graph, removing_an_unknown_id_changes_nothing) {
  Graph g;
  g.add_node(1, NodeEntry{"n", "/", "/"});
  EXPECT_FALSE(g.remove(1, 999));
  EXPECT_EQ(g.size(), 1u);
}

TEST(Graph, a_topic_with_two_types_lists_both) {
  Graph g;
  g.add_endpoint(1, ep(EndpointKind::kPublisher, "/x", "a/msg/A"));
  g.add_endpoint(2, ep(EndpointKind::kSubscription, "/x", "b/msg/B"));
  auto nt = g.names_and_types({EndpointKind::kPublisher, EndpointKind::kSubscription});
  ASSERT_EQ(nt.size(), 1u);
  EXPECT_EQ(nt["/x"], (std::set<std::string>{"a/msg/A", "b/msg/B"}));
}

TEST(Graph, names_and_types_by_node_keep_only_that_nodes_endpoints) {
  Graph g;
  g.add_endpoint(1, ep(EndpointKind::kPublisher, "/a", "t/msg/T", "talker", "/"));
  g.add_endpoint(1, ep(EndpointKind::kPublisher, "/b", "t/msg/T", "other", "/"));
  g.add_endpoint(1, ep(EndpointKind::kPublisher, "/c", "t/msg/T", "talker", "/ns"));
  const std::string name = "talker", ns = "/";
  auto nt = g.names_and_types({EndpointKind::kPublisher}, &name, &ns);
  ASSERT_EQ(nt.size(), 1u);
  EXPECT_TRUE(nt.count("/a"));
}

TEST(Graph, services_do_not_appear_among_topics) {
  Graph g;
  g.add_endpoint(1, ep(EndpointKind::kService, "/add", "e/srv/AddTwoInts"));
  EXPECT_TRUE(g.names_and_types({EndpointKind::kPublisher, EndpointKind::kSubscription}).empty());
}

// P8: after an owner goes, no view mentions anything it registered, and everything else stays.
TEST(GraphProperty, removing_an_owner_removes_exactly_its_entries) {
  std::mt19937 rng(11);
  for (int round = 0; round < 100; round++) {
    Graph g;
    std::vector<size_t> per_owner(4, 0);
    for (int i = 0; i < 40; i++) {
      const Graph::Owner o = rng() % 4;
      if (rng() % 4 == 0) {
        g.add_node(o, NodeEntry{"node" + std::to_string(o), "/", "/"});
      } else {
        g.add_endpoint(o, ep(EndpointKind(rng() % 4), "/t" + std::to_string(rng() % 3), "x/msg/X",
                             "node" + std::to_string(o)));
      }
      per_owner[o]++;
    }
    const Graph::Owner gone = rng() % 4;
    const size_t before = g.size();
    ASSERT_EQ(g.remove_owner(gone), per_owner[gone]);
    ASSERT_EQ(g.size(), before - per_owner[gone]);
    ASSERT_FALSE(g.node_exists("node" + std::to_string(gone), "/"));
    for (auto k : {EndpointKind::kPublisher, EndpointKind::kSubscription, EndpointKind::kService,
                   EndpointKind::kClient})
      for (int t = 0; t < 3; t++)
        for (const Endpoint &e : g.endpoints(k, "/t" + std::to_string(t)))
          ASSERT_NE(e.node_name, "node" + std::to_string(gone));
  }
}

// P9: counts equal the number of matching endpoints registered, for random graphs.
TEST(GraphProperty, counts_match_a_model) {
  std::mt19937 rng(12);
  for (int round = 0; round < 100; round++) {
    Graph g;
    std::map<std::pair<int, std::string>, size_t> model;
    for (int i = 0; i < 50; i++) {
      const int k = int(rng() % 4);
      const std::string name = "/t" + std::to_string(rng() % 5);
      g.add_endpoint(rng() % 3, ep(EndpointKind(k), name));
      model[{k, name}]++;
    }
    for (int k = 0; k < 4; k++)
      for (int t = 0; t < 5; t++) {
        const std::string name = "/t" + std::to_string(t);
        const std::pair<int, std::string> key{k, name};
        ASSERT_EQ(g.count(EndpointKind(k), name), model[key]);
      }
  }
}
