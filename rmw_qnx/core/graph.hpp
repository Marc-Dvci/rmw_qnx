// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// The ROS graph as the /dev/ros2 resource manager keeps it: nodes and endpoints, each registered by an owner (the
// connection that registered it). When an owner goes (its process closed the connection, or died), everything it
// registered goes with it. Pure bookkeeping, no QNX header: the resource manager wraps it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
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
  // reliability, durability, history, depth, liveliness: the rmw enum values, carried as numbers.
  int64_t qos[5] = {0, 0, 0, 0, 0};
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

// One process's (one context's) part of the graph as a text file, so other processes can read it with no server:
//   C <tab> pid <tab> channel            the context, pulsed when the graph changes
//   N <tab> name <tab> namespace <tab> enclave
//   E <tab> kind <tab> name <tab> type <tab> node <tab> namespace <tab> gid hex <tab> qos x5
// Fields are percent-escaped for %, tab and newline, so any name survives.
inline std::string escape_field(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '%' || c == '\t' || c == '\n' || c == '\r') {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", unsigned(static_cast<unsigned char>(c)));
      out += b;
    } else {
      out += c;
    }
  }
  return out;
}

inline bool unescape_field(const std::string &s, std::string *out) {
  out->clear();
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '%') {
      *out += s[i];
      continue;
    }
    if (i + 2 >= s.size()) return false;  // a '%' needs two hex digits after it
    unsigned v = 0;
    for (int k = 1; k <= 2; k++) {
      const char c = s[i + k];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return false;
    }
    *out += char(v);
    i += 2;
  }
  return true;
}

struct GraphPart {
  int64_t pid = 0;
  int64_t chid = -1;
  std::vector<NodeEntry> nodes;
  std::vector<Endpoint> endpoints;
};

inline std::string write_part(const GraphPart &p) {
  std::ostringstream o;
  o << "C\t" << p.pid << "\t" << p.chid << "\n";
  for (const auto &n : p.nodes)
    o << "N\t" << escape_field(n.name) << "\t" << escape_field(n.ns) << "\t" << escape_field(n.enclave) << "\n";
  for (const auto &e : p.endpoints) {
    o << "E\t" << int(e.kind) << "\t" << escape_field(e.name) << "\t" << escape_field(e.type) << "\t"
      << escape_field(e.node_name) << "\t" << escape_field(e.node_namespace) << "\t";
    for (uint8_t b : e.gid) {
      char h[3];
      std::snprintf(h, sizeof h, "%02x", b);
      o << h;
    }
    for (int64_t q : e.qos) o << "\t" << q;
    o << "\n";
  }
  return o.str();
}

inline std::vector<std::string> split_tabs(const std::string &line) {
  std::vector<std::string> f;
  size_t at = 0;
  for (;;) {
    const size_t t = line.find('\t', at);
    f.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at));
    if (t == std::string::npos) return f;
    at = t + 1;
  }
}

// Parses a part; a malformed line (a file being written by an older version, or damaged) is skipped, never
// trusted. Returns false if the context line is missing.
inline bool read_part(const std::string &text, GraphPart *p) {
  *p = GraphPart{};
  bool have_context = false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto f = split_tabs(line);
    if (f[0] == "C" && f.size() == 3) {
      try {
        p->pid = std::stoll(f[1]);
        p->chid = std::stoll(f[2]);
        have_context = true;
      } catch (...) {
      }
    } else if (f[0] == "N" && f.size() == 4) {
      NodeEntry n;
      if (unescape_field(f[1], &n.name) && unescape_field(f[2], &n.ns) && unescape_field(f[3], &n.enclave))
        p->nodes.push_back(std::move(n));
    } else if (f[0] == "E" && f.size() == 12) {
      Endpoint e{};
      int kind = -1;
      try {
        kind = std::stoi(f[1]);
        for (int k = 0; k < 5; k++) e.qos[k] = std::stoll(f[7 + k]);
      } catch (...) {
        continue;
      }
      if (kind < 0 || kind > 3 || f[6].size() != 32) continue;
      e.kind = EndpointKind(kind);
      bool hex_ok = true;
      for (int k = 0; k < 16 && hex_ok; k++) {
        unsigned v = 0;
        hex_ok = std::sscanf(f[6].c_str() + 2 * k, "%2x", &v) == 1;
        e.gid[k] = uint8_t(v);
      }
      if (hex_ok && unescape_field(f[2], &e.name) && unescape_field(f[3], &e.type) &&
          unescape_field(f[4], &e.node_name) && unescape_field(f[5], &e.node_namespace))
        p->endpoints.push_back(std::move(e));
    }
  }
  return have_context;
}

}  // namespace rmw_qnx::core
