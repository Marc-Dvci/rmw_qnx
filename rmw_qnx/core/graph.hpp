// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// The ROS graph as the /dev/ros2 resource manager keeps it: nodes and endpoints, each registered by an owner (the
// connection that registered it). When an owner goes (its process closed the connection, or died), everything it
// registered goes with it. Pure bookkeeping, no QNX header: the resource manager wraps it.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace rmw_qnx::core {

enum class EndpointKind { kPublisher, kSubscription, kService, kClient };

struct Endpoint {
  EndpointKind kind;
  std::string name;  // fully qualified topic or service name
  std::string type;  // e.g. std_msgs/msg/String
  std::string node_name;
  std::string node_namespace;
  uint8_t gid[16];
};

struct NodeEntry {
  std::string name;
  std::string ns;
  std::string enclave;
};

class Graph {
 public:
  using Owner = uint64_t;
  using Id = uint64_t;

  Id add_node(Owner o, NodeEntry n) {
    const Id id = next_++;
    nodes_[id] = {o, std::move(n)};
    return id;
  }
  Id add_endpoint(Owner o, Endpoint e) {
    const Id id = next_++;
    endpoints_[id] = {o, std::move(e)};
    return id;
  }
  // Removes one entry if `o` owns it. Returns whether something was removed.
  bool remove(Owner o, Id id) {
    if (auto it = nodes_.find(id); it != nodes_.end() && it->second.first == o) {
      nodes_.erase(it);
      return true;
    }
    if (auto it = endpoints_.find(id); it != endpoints_.end() && it->second.first == o) {
      endpoints_.erase(it);
      return true;
    }
    return false;
  }
  // Removes everything `o` registered. Returns how many entries went.
  size_t remove_owner(Owner o) {
    size_t n = 0;
    for (auto it = nodes_.begin(); it != nodes_.end();) {
      if (it->second.first == o) {
        it = nodes_.erase(it);
        n++;
      } else {
        ++it;
      }
    }
    for (auto it = endpoints_.begin(); it != endpoints_.end();) {
      if (it->second.first == o) {
        it = endpoints_.erase(it);
        n++;
      } else {
        ++it;
      }
    }
    return n;
  }

  size_t count(EndpointKind k, const std::string &name) const {
    size_t n = 0;
    for (const auto &kv : endpoints_) n += kv.second.second.kind == k && kv.second.second.name == name;
    return n;
  }

  std::vector<Endpoint> endpoints(EndpointKind k, const std::string &name) const {
    std::vector<Endpoint> out;
    for (const auto &kv : endpoints_)
      if (kv.second.second.kind == k && kv.second.second.name == name) out.push_back(kv.second.second);
    return out;
  }

  // Topic (or service) name -> sorted set of types, over the given kinds; optionally only one node's endpoints.
  std::map<std::string, std::set<std::string>> names_and_types(std::set<EndpointKind> kinds,
                                                               const std::string *node_name = nullptr,
                                                               const std::string *node_ns = nullptr) const {
    std::map<std::string, std::set<std::string>> out;
    for (const auto &kv : endpoints_) {
      const Endpoint &e = kv.second.second;
      if (!kinds.count(e.kind)) continue;
      if (node_name && (e.node_name != *node_name || e.node_namespace != *node_ns)) continue;
      out[e.name].insert(e.type);
    }
    return out;
  }

  std::vector<NodeEntry> nodes() const {
    std::vector<NodeEntry> out;
    for (const auto &kv : nodes_) out.push_back(kv.second.second);
    return out;
  }

  bool node_exists(const std::string &name, const std::string &ns) const {
    for (const auto &kv : nodes_)
      if (kv.second.second.name == name && kv.second.second.ns == ns) return true;
    return false;
  }

  size_t size() const { return nodes_.size() + endpoints_.size(); }

 private:
  Id next_ = 1;
  std::map<Id, std::pair<Owner, NodeEntry>> nodes_;
  std::map<Id, std::pair<Owner, Endpoint>> endpoints_;
};

}  // namespace rmw_qnx::core
