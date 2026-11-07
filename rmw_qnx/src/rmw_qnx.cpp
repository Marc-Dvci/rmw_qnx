// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// rmw_qnx: the ROS 2 middleware interface over QNX message passing.
//
// This first cut carries services: init, nodes, guard conditions, wait sets, clients and services. A service request
// travels by MsgSend and its response by MsgReply, so the thread that serves it runs at the priority of the thread
// that called rmw_send_request. Topics, the /dev/ros2 graph and QoS events come next; until then their entry points
// return RMW_RET_UNSUPPORTED rather than pretend.
//
// Priority path of a request:
//   caller thread (P) --MsgSendPulse(prio -1 = P)--> client's sender thread, which now runs at P
//   sender thread (P) --MsgSend--> context channel of the server; the thread blocked in MsgReceive inside rmw_wait
//   takes priority P, returns to the executor, runs the callback, and rmw_send_response does MsgReply.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cdr.hpp"
#include "graph.hpp"
#include "names.hpp"
#include "qos.hpp"
#include "sendq.hpp"
#include "rmw/time.h"
#include "rcutils/strdup.h"
#include "rmw/allocators.h"
#include "rmw/dynamic_message_type_support.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/features.h"
#include "rmw/get_network_flow_endpoints.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h"
#include "rmw/serialized_message.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_c/service_introspection.h"
#include "rmw/validate_full_topic_name.h"
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"
#include "serdes.hpp"
#include "topics.hpp"

namespace its = rosidl_typesupport_introspection_cpp;
using rmw_qnx::core::CdrReader;
using rmw_qnx::core::CdrWriter;

namespace {

const char *const kId = "rmw_qnx";
constexpr int kPulseWake = _PULSE_CODE_MINAVAIL;
constexpr int kPulseSend = _PULSE_CODE_MINAVAIL + 1;
constexpr int kPulseQuit = _PULSE_CODE_MINAVAIL + 2;
constexpr uint32_t kReqMagic = 0x52514d52;  // "RMQR"
constexpr uint32_t kRepMagic = 0x50514d52;  // "RMQP"
constexpr size_t kMaxReply = 64 * 1024;

struct ReqHdr {
  uint32_t magic;
  uint32_t service;  // index in the server's context
  int64_t seq;
  uint8_t guid[RMW_GID_STORAGE_SIZE];
  uint32_t len;
  uint32_t reserved;
};

struct RepHdr {
  uint32_t magic;
  int32_t status;
  int64_t seq;
  uint32_t len;
  uint32_t reserved;
};

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Something a wait set can wait for: it pulses every channel currently waiting on it. Each pulse carries the
// waiting thread's own assigned priority, so a wake-up never lends the waker's priority to the waiter; only a
// service request (a message, not a pulse) carries the sender's priority.
struct Waitable {
  std::mutex mu;
  std::vector<std::pair<int, int>> waiters;  // (connection id to the waiting channel, waiter's priority)
  void add(int coid, int prio) {
    std::lock_guard<std::mutex> l(mu);
    waiters.emplace_back(coid, prio);
  }
  void remove(int coid, int prio) {
    std::lock_guard<std::mutex> l(mu);
    auto it = std::find(waiters.begin(), waiters.end(), std::make_pair(coid, prio));
    if (it != waiters.end()) waiters.erase(it);
  }
  void notify(int except_coid = -1) {
    std::lock_guard<std::mutex> l(mu);
    for (const auto &w : waiters)
      if (w.first != except_coid) MsgSendPulse(w.first, w.second, kPulseWake, 0);
  }
};

int assigned_priority() {
  sched_param p{};
  return SchedGet(0, 0, &p) == -1 ? 10 : p.sched_priority;
}

struct GuardCondition {
  std::atomic<bool> triggered{false};
  Waitable w;
};

// A QoS event of one endpoint. ev->data of an rmw_event_t points to one of these, which lives in the endpoint. A
// deadline event is armed with a QNX timer whose pulse (at the priority of the thread that created the event) is
// received by the context's relay thread; no thread polls. The counts are computed from the clock when asked, so
// a late pulse never changes them: missed = periods elapsed since the last publication, counted once each.
int64_t mono_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

struct EventState {
  enum Kind { kNone, kDeadline, kMatched } kind = kNone;
  uint32_t id = 0;
  std::mutex mu;
  Waitable w;
  // deadline
  int64_t deadline_ns = 0, last_ns = 0, folded = 0, reported = 0;
  timer_t timer = 0;
  bool timer_on = false;
  // matched
  rmw_qnx::Topic *topic = nullptr;
  bool counts_subs = false;
  uint32_t joins_seen = 0;
  int64_t reported_current = 0, total = 0;

  int64_t missed_locked(int64_t now) const { return deadline_ns > 0 ? folded + (now - last_ns) / deadline_ns : 0; }
  void touch() {  // a publication: the periods that elapsed before it are missed for good
    if (kind != kDeadline || deadline_ns <= 0) return;
    std::lock_guard<std::mutex> l(mu);
    const int64_t now = mono_ns();
    folded += (now - last_ns) / deadline_ns;
    last_ns = now;
  }
  uint32_t joins() const { return counts_subs ? topic->sub_joins() : topic->pub_joins(); }
  int64_t current() const { return counts_subs ? int64_t(topic->subscribers()) : int64_t(topic->publishers()); }
  bool ready() {
    std::lock_guard<std::mutex> l(mu);
    if (kind == kDeadline) return missed_locked(mono_ns()) > reported;
    if (kind == kMatched && topic) return joins() != joins_seen || current() != reported_current;
    return false;
  }
};

struct ServiceImpl;

}  // namespace

struct SubscriptionImpl;

struct rmw_context_impl_s {
  int chid = -1;
  int self_coid = -1;
  std::string dir;  // service registry of this domain
  size_t domain = 0;
  std::mutex mu;
  std::map<uint32_t, ServiceImpl *> services;
  uint32_t next_service = 1;
  std::atomic<uint64_t> next_gid{1};
  // Topic pulses from publishers in any process arrive on topic_chid. The relay thread takes each pulse's priority
  // (the subscriber's), marks the subscription and wakes whatever waits on it.
  int topic_chid = -1, topic_coid = -1;
  std::thread relay;
  std::map<uint32_t, SubscriptionImpl *> subs;
  uint32_t next_sub = 1;
  bool shutdown = false;
  // This context's part of the graph, written to graph_path for every process to read (core/graph.hpp).
  std::mutex graph_mu;
  std::string graph_path;
  std::map<uint64_t, rmw_qnx::core::NodeEntry> my_nodes;
  std::map<uint64_t, rmw_qnx::core::Endpoint> my_endpoints;
  uint64_t next_graph_id = 1;
  std::vector<GuardCondition *> graph_gcs;  // of this context's nodes, triggered on every graph change
  std::mutex ev_mu;
  std::map<uint32_t, EventState *> events;  // live QoS events, by the id their timer pulse carries
  uint32_t next_event = 1;
};

struct SubscriptionImpl {
  rmw_context_impl_s *ctx = nullptr;
  uint32_t id = 0;
  std::string topic_name;
  std::unique_ptr<rmw_qnx::Topic> topic;  // null for types without C++ introspection: nothing is carried
  int slot = -1;
  const its::MessageMembers *members = nullptr;
  rmw_qos_profile_t qos{};
  uint8_t gid[RMW_GID_STORAGE_SIZE] = {};
  std::mutex mu;
  uint64_t graph_id = 0;
  EventState matched_ev, inert_ev;
  uint64_t pos = 0;  // next ring sequence this subscription takes
  Waitable w;
  bool has_data() {
    if (!topic) return false;
    std::lock_guard<std::mutex> l(mu);
    return topic->ring().head() > pos;
  }
};

namespace {

constexpr int kPulseTopic = _PULSE_CODE_MINAVAIL + 3;
constexpr int kPulseGraph = _PULSE_CODE_MINAVAIL + 4;

void trigger_graph(rmw_context_impl_s *ctx) {
  std::lock_guard<std::mutex> l(ctx->graph_mu);
  for (GuardCondition *g : ctx->graph_gcs) {
    g->triggered = true;
    g->w.notify();
  }
  // Matched events look at the endpoint counts: wake them too.
  std::lock_guard<std::mutex> l2(ctx->ev_mu);
  for (auto &kv : ctx->events)
    if (kv.second->kind == EventState::kMatched) kv.second->w.notify();
}

constexpr int kPulseDeadline = _PULSE_CODE_MINAVAIL + 5;

// Arms the one-shot timer for the next deadline boundary after the last publication. Call with es->mu held.
void deadline_arm_locked(EventState *es) {
  const int64_t now = mono_ns();
  const int64_t next = es->last_ns + ((now - es->last_ns) / es->deadline_ns + 1) * es->deadline_ns;
  itimerspec its{};
  its.it_value.tv_sec = next / 1000000000;
  its.it_value.tv_nsec = next % 1000000000;
  timer_settime(es->timer, TIMER_ABSTIME, &its, nullptr);
}

// The timer pulse arrived (relay thread, holding ev_mu): wake waiters if a period was missed, and arm the next.
void deadline_fire(EventState *es) {
  bool missed;
  {
    std::lock_guard<std::mutex> l(es->mu);
    if (!es->timer_on) return;
    missed = es->missed_locked(mono_ns()) > es->reported;
    deadline_arm_locked(es);
  }
  if (missed) es->w.notify();
}

// Registers an event of an endpoint once; later calls for the same event do nothing.
void event_setup(rmw_context_impl_s *ctx, EventState *es, EventState::Kind kind, rmw_qnx::Topic *topic,
                 bool counts_subs, uint64_t deadline_ns) {
  if (es->kind != EventState::kNone) return;
  es->kind = kind;
  if (kind == EventState::kMatched) {
    es->topic = topic;
    es->counts_subs = counts_subs;
    if (topic) {  // matches that exist when the event is created are not reported; later changes are
      es->joins_seen = es->joins();
      es->reported_current = es->current();
      es->total = es->reported_current;
    }
  }
  {
    std::lock_guard<std::mutex> l(ctx->ev_mu);
    es->id = ctx->next_event++;
    ctx->events[es->id] = es;
  }
  if (kind == EventState::kDeadline && deadline_ns > 0) {
    std::lock_guard<std::mutex> l(es->mu);
    es->deadline_ns = int64_t(deadline_ns);
    es->last_ns = mono_ns();
    sigevent sev;
    SIGEV_PULSE_INIT(&sev, ctx->topic_coid, assigned_priority(), kPulseDeadline, int(es->id));
    if (timer_create(CLOCK_MONOTONIC, &sev, &es->timer) == 0) {
      es->timer_on = true;
      deadline_arm_locked(es);
    }
  }
}

// The state of an rmw_event_t, or null for one never initialised here (a zero-initialised event is never ready).
EventState *event_state_of(void *event) {
  auto *ev = static_cast<rmw_event_t *>(event);
  return ev && ev->implementation_identifier == kId ? static_cast<EventState *>(ev->data) : nullptr;
}

void event_teardown(rmw_context_impl_s *ctx, EventState *es) {
  if (es->kind == EventState::kNone) return;
  {
    std::lock_guard<std::mutex> l(ctx->ev_mu);
    ctx->events.erase(es->id);
  }
  std::lock_guard<std::mutex> l(es->mu);
  if (es->timer_on) timer_delete(es->timer);
  es->timer_on = false;
}

void relay_main(rmw_context_impl_s *ctx) {
  for (;;) {
    _pulse p;
    if (MsgReceivePulse(ctx->topic_chid, &p, sizeof p, nullptr) == -1) continue;
    if (p.code == kPulseQuit) return;
    if (p.code == kPulseDeadline) {
      std::lock_guard<std::mutex> l(ctx->ev_mu);
      auto it = ctx->events.find(uint32_t(p.value.sival_int));
      if (it != ctx->events.end()) deadline_fire(it->second);
      continue;
    }
    if (p.code == kPulseGraph) {
      trigger_graph(ctx);
      continue;
    }
    if (p.code != kPulseTopic) continue;
    // This thread now runs at the priority the subscriber registered with.
    std::lock_guard<std::mutex> l(ctx->mu);
    auto it = ctx->subs.find(uint32_t(p.value.sival_int));
    if (it != ctx->subs.end()) it->second->w.notify();
  }
}

void make_gid(rmw_context_impl_s *ctx, uint8_t *gid) {
  std::memset(gid, 0, RMW_GID_STORAGE_SIZE);
  const uint32_t pid = static_cast<uint32_t>(getpid());
  const uint64_t n = ctx->next_gid.fetch_add(1);
  std::memcpy(gid, "QNX", 3);
  std::memcpy(gid + 4, &pid, 4);
  std::memcpy(gid + 8, &n, 8);
}

struct Pending {
  rcvid_t rcvid;
  int64_t seq;
  uint8_t guid[RMW_GID_STORAGE_SIZE];
  int64_t received_ns;
  std::vector<uint8_t> data;
};

struct ServiceImpl {
  rmw_context_impl_s *ctx;
  uint32_t index;
  const its::MessageMembers *req;
  const its::MessageMembers *res;
  std::string reg_path;
  std::mutex mu;
  std::deque<Pending> queue;
  std::map<std::pair<std::string, int64_t>, rcvid_t> inflight;
  Waitable w;
  uint64_t graph_id = 0;
};

struct Response {
  int64_t seq;
  int64_t received_ns;
  std::vector<uint8_t> data;
};

struct ClientImpl {
  rmw_context_impl_s *ctx;
  std::string reg_path;
  const its::MessageMembers *req;
  const its::MessageMembers *res;
  uint8_t gid[RMW_GID_STORAGE_SIZE];
  std::atomic<int64_t> next_seq{1};
  std::mutex mu;
  rmw_qnx::core::SendQueue to_send;  // each request with its caller's priority
  std::deque<Response> responses;
  Waitable w;
  int chid = -1, coid = -1;
  std::thread sender;
  // Server connection, used by the sender thread only.
  int srv_coid = -1;
  pid_t srv_pid = 0;
  int srv_chid = -1;
  uint32_t srv_index = 0;
  uint64_t graph_id = 0;
};

struct WaitSetImpl {
  rmw_context_impl_s *ctx;
  int chid = -1, coid = -1;  // private channel, used when the wait set holds no service
};

struct NodeImpl {
  GuardCondition graph;  // triggered on every change to the graph of this domain
  rmw_guard_condition_t handle;
  uint64_t graph_id = 0;
};

bool read_registry(const std::string &path, pid_t *pid, int *chid, uint32_t *index) {
  std::ifstream f(path);
  long p = 0, c = 0;
  unsigned long i = 0;
  if (!(f >> p >> c >> i)) return false;
  *pid = static_cast<pid_t>(p);
  *chid = static_cast<int>(c);
  *index = static_cast<uint32_t>(i);
  return kill(*pid, 0) == 0;
}

void mkdirs(const std::string &path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); i++) {
    cur += path[i];
    if (path[i] == '/' && i > 0) mkdir(cur.c_str(), 0777);
  }
  mkdir(path.c_str(), 0777);
}

std::string registry_path(rmw_context_impl_s *ctx, const char *service_name) {
  return ctx->dir + "/" + rmw_qnx::core::encode_name(service_name);
}

// ---- graph ----
//
// Each context writes its nodes and endpoints to /tmp/rmwqnx/d<domain>/graph/<pid>.<n> (written to a temporary
// name and renamed, so a reader sees a whole file). A query reads every file whose process is alive. After each
// change the writer pulses every other context listed, whose relay thread then triggers its nodes' graph guard
// conditions; ros2graphd reads the same files for /dev/ros2/nodes.

std::string graph_dir(size_t domain) { return "/tmp/rmwqnx/d" + std::to_string(domain) + "/graph"; }

std::vector<rmw_qnx::core::GraphPart> read_graph(size_t domain) {
  std::vector<rmw_qnx::core::GraphPart> out;
  const std::string dir = graph_dir(domain);
  DIR *d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.' || n.find(".tmp") != std::string::npos) continue;
    std::ifstream f(dir + "/" + n);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    rmw_qnx::core::GraphPart p;
    if (!rmw_qnx::core::read_part(text, &p)) continue;
    if (p.pid != getpid() && kill(pid_t(p.pid), 0) != 0 && errno == ESRCH) continue;  // its process is gone
    out.push_back(std::move(p));
  }
  closedir(d);
  return out;
}

rmw_qnx::core::Graph load_graph(size_t domain) {
  rmw_qnx::core::Graph g;
  for (auto &p : read_graph(domain)) {
    for (auto &n : p.nodes) g.add_node(uint64_t(p.pid), n);
    for (auto &e : p.endpoints) g.add_endpoint(uint64_t(p.pid), e);
  }
  return g;
}

void publish_graph(rmw_context_impl_s *ctx) {
  rmw_qnx::core::GraphPart part;
  part.pid = getpid();
  part.chid = ctx->topic_chid;
  {
    std::lock_guard<std::mutex> l(ctx->graph_mu);
    for (auto &kv : ctx->my_nodes) part.nodes.push_back(kv.second);
    for (auto &kv : ctx->my_endpoints) part.endpoints.push_back(kv.second);
  }
  const std::string tmp = ctx->graph_path + ".tmp";
  {
    std::ofstream f(tmp);
    f << rmw_qnx::core::write_part(part);
  }
  rename(tmp.c_str(), ctx->graph_path.c_str());
  trigger_graph(ctx);
  for (const auto &p : read_graph(ctx->domain)) {
    if (p.pid == part.pid && p.chid == part.chid) continue;
    const int coid = ConnectAttach(0, pid_t(p.pid), int(p.chid), _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) continue;
    MsgSendPulse(coid, -1, kPulseGraph, 0);
    ConnectDetach(coid);
  }
}

uint64_t graph_add_node(rmw_context_impl_s *ctx, const char *name, const char *ns, GuardCondition *gc) {
  uint64_t id;
  {
    std::lock_guard<std::mutex> l(ctx->graph_mu);
    id = ctx->next_graph_id++;
    ctx->my_nodes[id] = rmw_qnx::core::NodeEntry{name, ns, "/"};
    ctx->graph_gcs.push_back(gc);
  }
  publish_graph(ctx);
  return id;
}

uint64_t graph_add_endpoint(const rmw_node_t *node, rmw_qnx::core::EndpointKind kind, const char *name,
                            const std::string &type, const uint8_t *gid, const rmw_qos_profile_t &q) {
  rmw_context_impl_s *ctx = node->context->impl;
  rmw_qnx::core::Endpoint e{};
  e.kind = kind;
  e.name = name;
  e.type = type;
  e.node_name = node->name;
  e.node_namespace = node->namespace_;
  std::memcpy(e.gid, gid, 16);
  e.qos[0] = q.reliability;
  e.qos[1] = q.durability;
  e.qos[2] = q.history;
  e.qos[3] = int64_t(q.depth);
  e.qos[4] = q.liveliness;
  uint64_t id;
  {
    std::lock_guard<std::mutex> l(ctx->graph_mu);
    id = ctx->next_graph_id++;
    ctx->my_endpoints[id] = e;
  }
  publish_graph(ctx);
  return id;
}

void graph_remove(rmw_context_impl_s *ctx, uint64_t id, GuardCondition *gc = nullptr) {
  {
    std::lock_guard<std::mutex> l(ctx->graph_mu);
    ctx->my_nodes.erase(id);
    ctx->my_endpoints.erase(id);
    if (gc) ctx->graph_gcs.erase(std::remove(ctx->graph_gcs.begin(), ctx->graph_gcs.end(), gc), ctx->graph_gcs.end());
  }
  publish_graph(ctx);
}

std::string ros_type(std::string ns, const std::string &name) {
  for (const char *sep : {"::", "__"})
    for (size_t i; (i = ns.find(sep)) != std::string::npos;) ns.replace(i, 2, "/");
  return ns + "/" + name;
}

// The ROS type name of a message or service type support, from whichever introspection it offers.
std::string message_type_name(const rosidl_message_type_support_t *ts) {
  std::string out;
  if (const auto *h = get_message_typesupport_handle(ts, its::typesupport_identifier)) {
    const auto *m = static_cast<const its::MessageMembers *>(h->data);
    out = ros_type(m->message_namespace_, m->message_name_);
  } else if (const auto *c = get_message_typesupport_handle(ts, "rosidl_typesupport_introspection_c")) {
    const auto *m = static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(c->data);
    out = ros_type(m->message_namespace_, m->message_name_);
  }
  rmw_reset_error();
  return out;
}

std::string service_type_name(const rosidl_service_type_support_t *ts) {
  std::string out;
  if (const auto *h = get_service_typesupport_handle(ts, its::typesupport_identifier)) {
    const auto *m = static_cast<const its::ServiceMembers *>(h->data);
    out = ros_type(m->service_namespace_, m->service_name_);
  } else if (const auto *c = get_service_typesupport_handle(ts, "rosidl_typesupport_introspection_c")) {
    const auto *m = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(c->data);
    out = ros_type(m->service_namespace_, m->service_name_);
  }
  rmw_reset_error();
  return out;
}

void sender_main(ClientImpl *c) {
  std::vector<uint8_t> reply(sizeof(RepHdr) + kMaxReply);
  for (;;) {
    _pulse pulse;
    if (MsgReceivePulse(c->chid, &pulse, sizeof pulse, nullptr) == -1) continue;
    if (pulse.code == kPulseQuit) return;
    if (pulse.code != kPulseSend) continue;
    // This thread now runs at the priority of the caller that sent the pulse, which the pulse also names. Pending
    // pulses arrive highest priority first, so take the oldest request of that priority, not the oldest request.
    rmw_qnx::core::Outgoing item;
    {
      std::lock_guard<std::mutex> l(c->mu);
      auto o = c->to_send.take(pulse.value.sival_int);
      if (!o) continue;
      item = std::move(*o);
    }
    pid_t pid;
    int chid;
    uint32_t index;
    if (!read_registry(c->reg_path, &pid, &chid, &index)) continue;  // no server: the request is dropped
    if (c->srv_coid == -1 || pid != c->srv_pid || chid != c->srv_chid) {
      if (c->srv_coid != -1) ConnectDetach(c->srv_coid);
      c->srv_coid = ConnectAttach(0, pid, chid, _NTO_SIDE_CHANNEL, 0);
      if (c->srv_coid == -1) continue;
      c->srv_pid = pid;
      c->srv_chid = chid;
    }
    ReqHdr h{};
    h.magic = kReqMagic;
    h.service = index;
    h.seq = item.seq;
    std::memcpy(h.guid, c->gid, RMW_GID_STORAGE_SIZE);
    h.len = static_cast<uint32_t>(item.bytes.size());
    iov_t siov[2], riov[1];
    SETIOV(&siov[0], &h, sizeof h);
    SETIOV(&siov[1], item.bytes.data(), item.bytes.size());
    SETIOV(&riov[0], reply.data(), reply.size());
    if (MsgSendv(c->srv_coid, siov, 2, riov, 1) == -1) {
      ConnectDetach(c->srv_coid);
      c->srv_coid = -1;
      continue;
    }
    RepHdr rh;
    std::memcpy(&rh, reply.data(), sizeof rh);
    if (rh.magic != kRepMagic || rh.status != 0 || rh.len > kMaxReply) continue;
    {
      std::lock_guard<std::mutex> l(c->mu);
      c->responses.push_back(Response{rh.seq, now_ns(),
                                      std::vector<uint8_t>(reply.begin() + sizeof rh,
                                                           reply.begin() + sizeof rh + rh.len)});
    }
    c->w.notify();
  }
}

// A request has arrived on the context channel: queue it on its service. The receiving thread keeps the client's
// priority until it next receives. Waits on the channel the request arrived on are not pulsed: the thread that
// received it returns from rmw_wait anyway, and a pulse to itself, queued at the priority it had before this request,
// would raise it again (a server that just took a request from priority 10 would run it at 20).
void accept_request(rmw_context_impl_s *ctx, rcvid_t rcvid, const _msg_info &info, const uint8_t *buf,
                    size_t got, int receiving_coid) {
  if (got < sizeof(ReqHdr)) {
    MsgError(rcvid, EINVAL);
    return;
  }
  ReqHdr h;
  std::memcpy(&h, buf, sizeof h);
  if (h.magic != kReqMagic || sizeof(ReqHdr) + h.len != size_t(info.srcmsglen)) {
    MsgError(rcvid, EINVAL);
    return;
  }
  ServiceImpl *s = nullptr;
  {
    std::lock_guard<std::mutex> l(ctx->mu);
    auto it = ctx->services.find(h.service);
    if (it != ctx->services.end()) s = it->second;
  }
  if (!s) {
    MsgError(rcvid, ESRCH);
    return;
  }
  Pending p;
  p.rcvid = rcvid;
  p.seq = h.seq;
  std::memcpy(p.guid, h.guid, RMW_GID_STORAGE_SIZE);
  p.received_ns = now_ns();
  p.data.resize(h.len);
  const size_t have = got - sizeof h;
  std::memcpy(p.data.data(), buf + sizeof h, std::min<size_t>(have, h.len));
  if (have < h.len && MsgRead(rcvid, p.data.data() + have, h.len - have, sizeof h + have) == -1) {
    MsgError(rcvid, EIO);
    return;
  }
  {
    std::lock_guard<std::mutex> l(s->mu);
    s->queue.push_back(std::move(p));
  }
  s->w.notify(receiving_coid);
}

// The C++ introspection members of a service, or nullptr. A service that offers only the C type support (rcl's own
// ~/get_type_description is one) is still created: it is registered and visible, and a request to it gets an error
// reply, until the C introspection serialiser exists. *ok is false only if no usable type support exists at all.
const its::ServiceMembers *service_members(const rosidl_service_type_support_t *ts, bool *ok) {
  const rosidl_service_type_support_t *h = get_service_typesupport_handle(ts, its::typesupport_identifier);
  *ok = true;
  if (!h) {
    rmw_reset_error();
    if (get_service_typesupport_handle(ts, "rosidl_typesupport_introspection_c")) {
      rmw_reset_error();
      return nullptr;
    }
    rmw_reset_error();
    *ok = false;
    RMW_SET_ERROR_MSG("rmw_qnx: this service has no introspection type support");
    return nullptr;
  }
  return static_cast<const its::ServiceMembers *>(h->data);
}

#define CHECK_ID(ptr, ret)                                                                    \
  RMW_CHECK_ARGUMENT_FOR_NULL(ptr, ret);                                                      \
  RMW_CHECK_ARGUMENT_FOR_NULL((ptr)->implementation_identifier, ret);                         \
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(ptr, (ptr)->implementation_identifier, kId,                \
                                   return RMW_RET_INCORRECT_RMW_IMPLEMENTATION)

rmw_ret_t unsupported(const char *what) {
  RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_qnx: %s is not supported yet", what);
  return RMW_RET_UNSUPPORTED;
}

// Deserialises at the RMW boundary: a hostile buffer fails, and an allocation failure while filling the message is
// an error return, never an exception through rmw's C interface.
bool deserialize_safe(const its::MessageMembers *m, void *msg, const uint8_t *data, size_t len) {
  if (!m) return false;
  CdrReader r(data, len);
  if (!r.ok()) return false;
  try {
    return rmw_qnx::deserialize_cpp(m, msg, r);
  } catch (const std::exception &) {
    return false;
  }
}

rmw_qnx::core::Qos to_core(const rmw_qos_profile_t &q) {
  using namespace rmw_qnx::core;  // NOLINT
  Qos c;
  switch (q.reliability) {
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE: c.reliability = Reliability::kReliable; break;
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT: c.reliability = Reliability::kBestEffort; break;
    case RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT: c.reliability = Reliability::kSystemDefault; break;
    default: c.reliability = Reliability::kUnknown; break;
  }
  switch (q.durability) {
    case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL: c.durability = Durability::kTransientLocal; break;
    case RMW_QOS_POLICY_DURABILITY_VOLATILE: c.durability = Durability::kVolatile; break;
    case RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT: c.durability = Durability::kSystemDefault; break;
    default: c.durability = Durability::kUnknown; break;
  }
  switch (q.liveliness) {
    case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC: c.liveliness = Liveliness::kAutomatic; break;
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC: c.liveliness = Liveliness::kManualByTopic; break;
    case RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT: c.liveliness = Liveliness::kSystemDefault; break;
    default: c.liveliness = Liveliness::kUnknown; break;
  }
  auto ns = [](rmw_time_t t) -> uint64_t {
    if (rmw_time_equal(t, RMW_DURATION_INFINITE) || (t.sec == 0 && t.nsec == 0)) return 0;
    return static_cast<uint64_t>(rmw_time_total_nsec(t));
  };
  c.deadline_ns = ns(q.deadline);
  c.lease_ns = ns(q.liveliness_lease_duration);
  return c;
}

// A topic or service name rmw accepts: non-empty, and a valid fully qualified ROS name unless the QoS asks to avoid
// ROS's namespace conventions.
bool valid_name(const char *name, const rmw_qos_profile_t *qos) {
  if (!name || !*name) {
    RMW_SET_ERROR_MSG("empty name");
    return false;
  }
  if (qos->reliability == RMW_QOS_POLICY_RELIABILITY_UNKNOWN || qos->durability == RMW_QOS_POLICY_DURABILITY_UNKNOWN ||
      qos->history == RMW_QOS_POLICY_HISTORY_UNKNOWN || qos->liveliness == RMW_QOS_POLICY_LIVELINESS_UNKNOWN) {
    RMW_SET_ERROR_MSG("QoS profile holds unknown policies");
    return false;
  }
  if (qos->avoid_ros_namespace_conventions) return true;
  int result = 0;
  size_t index = 0;
  if (rmw_validate_full_topic_name(name, &result, &index) != RMW_RET_OK) return false;
  if (result != RMW_TOPIC_VALID) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid name: %s", rmw_full_topic_name_validation_result_string(result));
    return false;
  }
  return true;
}

}  // namespace

extern "C" {

const char *rmw_get_implementation_identifier(void) { return kId; }
const char *rmw_get_serialization_format(void) { return "cdr"; }

rmw_ret_t rmw_init_options_init(rmw_init_options_t *o, rcutils_allocator_t allocator) {
  RMW_CHECK_ARGUMENT_FOR_NULL(o, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ALLOCATOR(&allocator, return RMW_RET_INVALID_ARGUMENT);
  if (o->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  o->instance_id = 0;
  o->implementation_identifier = kId;
  o->allocator = allocator;
  o->impl = nullptr;
  o->enclave = nullptr;
  o->domain_id = RMW_DEFAULT_DOMAIN_ID;
  o->security_options = rmw_get_default_security_options();
  o->localhost_only = RMW_LOCALHOST_ONLY_DEFAULT;
  o->discovery_options = rmw_get_zero_initialized_discovery_options();
  return rmw_discovery_options_init(&o->discovery_options, 0, &allocator);
}

rmw_ret_t rmw_init_options_copy(const rmw_init_options_t *src, rmw_init_options_t *dst) {
  RMW_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);
  if (!src->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized src");
    return RMW_RET_INVALID_ARGUMENT;
  }
  CHECK_ID(src, RMW_RET_INVALID_ARGUMENT);
  if (dst->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized dst");
    return RMW_RET_INVALID_ARGUMENT;
  }
  rcutils_allocator_t alloc = src->allocator;
  const rcutils_allocator_t *a = &alloc;
  rmw_init_options_t tmp = *src;
  tmp.enclave = rcutils_strdup(src->enclave, *a);
  if (src->enclave && !tmp.enclave) return RMW_RET_BAD_ALLOC;
  tmp.security_options = rmw_get_zero_initialized_security_options();
  rmw_ret_t r = rmw_security_options_copy(&src->security_options, a, &tmp.security_options);
  if (r != RMW_RET_OK) {
    a->deallocate(tmp.enclave, a->state);
    return r;
  }
  tmp.discovery_options = rmw_get_zero_initialized_discovery_options();
  r = rmw_discovery_options_copy(&src->discovery_options, &alloc, &tmp.discovery_options);
  if (r != RMW_RET_OK) {
    rmw_security_options_fini(&tmp.security_options, a);
    a->deallocate(tmp.enclave, a->state);
    return r;
  }
  *dst = tmp;
  return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_fini(rmw_init_options_t *o) {
  RMW_CHECK_ARGUMENT_FOR_NULL(o, RMW_RET_INVALID_ARGUMENT);
  if (!o->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  CHECK_ID(o, RMW_RET_INVALID_ARGUMENT);
  rcutils_allocator_t *a = &o->allocator;
  RCUTILS_CHECK_ALLOCATOR(a, return RMW_RET_INVALID_ARGUMENT);
  a->deallocate(o->enclave, a->state);
  rmw_ret_t r = rmw_security_options_fini(&o->security_options, a);
  if (r != RMW_RET_OK) return r;
  r = rmw_discovery_options_fini(&o->discovery_options);
  *o = rmw_get_zero_initialized_init_options();
  return r;
}

rmw_ret_t rmw_init(const rmw_init_options_t *o, rmw_context_t *ctx) {
  RMW_CHECK_ARGUMENT_FOR_NULL(o, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx, RMW_RET_INVALID_ARGUMENT);
  if (!o->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  CHECK_ID(o, RMW_RET_INVALID_ARGUMENT);
  if (!o->enclave) {
    RMW_SET_ERROR_MSG("expected non-null enclave");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (ctx->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected a zero-initialized context");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto impl = std::make_unique<rmw_context_impl_s>();
  // Priority inheritance is the channel default: the receiving thread takes the sender's priority.
  impl->chid = ChannelCreate(0);
  if (impl->chid == -1) {
    RMW_SET_ERROR_MSG("rmw_qnx: ChannelCreate failed");
    return RMW_RET_ERROR;
  }
  impl->self_coid = ConnectAttach(0, 0, impl->chid, _NTO_SIDE_CHANNEL, 0);
  const size_t domain = o->domain_id == RMW_DEFAULT_DOMAIN_ID ? 0 : o->domain_id;
  impl->dir = "/tmp/rmwqnx/d" + std::to_string(domain) + "/services";
  impl->domain = domain;
  mkdirs(impl->dir);
  impl->topic_chid = ChannelCreate(0);
  impl->topic_coid = ConnectAttach(0, 0, impl->topic_chid, _NTO_SIDE_CHANNEL, 0);
  impl->relay = std::thread(relay_main, impl.get());
  static std::atomic<int> contexts{0};
  mkdirs(graph_dir(domain));
  impl->graph_path = graph_dir(domain) + "/" + std::to_string(getpid()) + "." + std::to_string(contexts++);
  ctx->instance_id = o->instance_id;
  ctx->implementation_identifier = kId;
  ctx->actual_domain_id = domain;
  ctx->options = rmw_get_zero_initialized_init_options();
  rmw_ret_t r = rmw_init_options_copy(o, &ctx->options);
  if (r != RMW_RET_OK) {
    MsgSendPulse(impl->topic_coid, -1, kPulseQuit, 0);
    impl->relay.join();
    ConnectDetach(impl->topic_coid);
    ChannelDestroy(impl->topic_chid);
    ConnectDetach(impl->self_coid);
    ChannelDestroy(impl->chid);
    *ctx = rmw_get_zero_initialized_context();
    return r;
  }
  ctx->impl = impl.release();
  return RMW_RET_OK;
}

rmw_ret_t rmw_shutdown(rmw_context_t *ctx) {
  CHECK_ID(ctx, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx->impl, RMW_RET_INVALID_ARGUMENT);
  ctx->impl->shutdown = true;
  return RMW_RET_OK;
}

rmw_ret_t rmw_context_fini(rmw_context_t *ctx) {
  CHECK_ID(ctx, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx->impl, RMW_RET_INVALID_ARGUMENT);
  if (!ctx->impl->shutdown) {
    RMW_SET_ERROR_MSG("context has not been shutdown");
    return RMW_RET_INVALID_ARGUMENT;
  }
  unlink(ctx->impl->graph_path.c_str());
  MsgSendPulse(ctx->impl->topic_coid, -1, kPulseQuit, 0);
  ctx->impl->relay.join();
  ConnectDetach(ctx->impl->topic_coid);
  ChannelDestroy(ctx->impl->topic_chid);
  ConnectDetach(ctx->impl->self_coid);
  ChannelDestroy(ctx->impl->chid);
  delete ctx->impl;
  rmw_ret_t r = rmw_init_options_fini(&ctx->options);
  *ctx = rmw_get_zero_initialized_context();
  return r;
}

// ---- nodes ----

rmw_node_t *rmw_create_node(rmw_context_t *ctx, const char *name, const char *ns) {
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(ctx, ctx->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx->impl, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ns, nullptr);
  if (ctx->impl->shutdown) {
    RMW_SET_ERROR_MSG("context has been shutdown");
    return nullptr;
  }
  int validation = 0;
  if (rmw_validate_node_name(name, &validation, nullptr) != RMW_RET_OK) return nullptr;
  if (validation != RMW_NODE_NAME_VALID) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node name: %s", rmw_node_name_validation_result_string(validation));
    return nullptr;
  }
  if (rmw_validate_namespace(ns, &validation, nullptr) != RMW_RET_OK) return nullptr;
  if (validation != RMW_NAMESPACE_VALID) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node namespace: %s", rmw_namespace_validation_result_string(validation));
    return nullptr;
  }
  rmw_node_t *node = rmw_node_allocate();
  if (!node) return nullptr;
  node->implementation_identifier = kId;
  node->name = rcutils_strdup(name, rcutils_get_default_allocator());
  node->namespace_ = rcutils_strdup(ns, rcutils_get_default_allocator());
  node->context = ctx;
  auto *impl = new NodeImpl();
  impl->handle.implementation_identifier = kId;
  impl->handle.data = &impl->graph;
  impl->handle.context = ctx;
  node->data = impl;
  impl->graph_id = graph_add_node(ctx->impl, name, ns, &impl->graph);
  return node;
}

rmw_ret_t rmw_destroy_node(rmw_node_t *node) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  auto *ni = static_cast<NodeImpl *>(node->data);
  graph_remove(node->context->impl, ni->graph_id, &ni->graph);
  rcutils_allocator_t a = rcutils_get_default_allocator();
  a.deallocate(const_cast<char *>(node->name), a.state);
  a.deallocate(const_cast<char *>(node->namespace_), a.state);
  delete ni;
  rmw_node_free(node);
  return RMW_RET_OK;
}

const rmw_guard_condition_t *rmw_node_get_graph_guard_condition(const rmw_node_t *node) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(node->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  return &static_cast<NodeImpl *>(node->data)->handle;
}

// ---- guard conditions ----

rmw_guard_condition_t *rmw_create_guard_condition(rmw_context_t *ctx) {
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(ctx, ctx->implementation_identifier, kId, return nullptr);
  auto *gc = new rmw_guard_condition_t;
  gc->implementation_identifier = kId;
  gc->data = new GuardCondition();
  gc->context = ctx;
  return gc;
}

rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t *gc) {
  CHECK_ID(gc, RMW_RET_ERROR);
  delete static_cast<GuardCondition *>(gc->data);
  delete gc;
  return RMW_RET_OK;
}

rmw_ret_t rmw_trigger_guard_condition(const rmw_guard_condition_t *gc) {
  CHECK_ID(gc, RMW_RET_INVALID_ARGUMENT);
  auto *g = static_cast<GuardCondition *>(gc->data);
  g->triggered = true;
  g->w.notify();
  return RMW_RET_OK;
}

// ---- wait sets ----

rmw_wait_set_t *rmw_create_wait_set(rmw_context_t *ctx, size_t) {
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(ctx, ctx->implementation_identifier, kId, return nullptr);
  auto *impl = new WaitSetImpl();
  impl->ctx = ctx->impl;
  impl->chid = ChannelCreate(_NTO_CHF_PRIVATE);
  impl->coid = ConnectAttach(0, 0, impl->chid, _NTO_SIDE_CHANNEL, 0);
  auto *ws = rmw_wait_set_allocate();
  ws->implementation_identifier = kId;
  ws->data = impl;
  ws->guard_conditions = nullptr;
  return ws;
}

rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t *ws) {
  CHECK_ID(ws, RMW_RET_ERROR);
  auto *impl = static_cast<WaitSetImpl *>(ws->data);
  ConnectDetach(impl->coid);
  ChannelDestroy(impl->chid);
  delete impl;
  rmw_wait_set_free(ws);
  return RMW_RET_OK;
}

rmw_ret_t rmw_wait(rmw_subscriptions_t *subs, rmw_guard_conditions_t *gcs, rmw_services_t *srvs,
                   rmw_clients_t *clis, rmw_events_t *evs, rmw_wait_set_t *ws, const rmw_time_t *timeout) {
  CHECK_ID(ws, RMW_RET_INVALID_ARGUMENT);
  auto *wsi = static_cast<WaitSetImpl *>(ws->data);
  rmw_context_impl_s *ctx = wsi->ctx;
  const bool has_services = srvs && srvs->service_count > 0;
  // Requests arrive on the context channel, so a wait set holding services receives there.
  const int chid = has_services ? ctx->chid : wsi->chid;
  const int coid = has_services ? ctx->self_coid : wsi->coid;

  auto for_each = [&](auto fn) {
    if (gcs)
      for (size_t i = 0; i < gcs->guard_condition_count; i++)
        fn(static_cast<GuardCondition *>(gcs->guard_conditions[i])->w);
    if (srvs)
      for (size_t i = 0; i < srvs->service_count; i++) fn(static_cast<ServiceImpl *>(srvs->services[i])->w);
    if (clis)
      for (size_t i = 0; i < clis->client_count; i++) fn(static_cast<ClientImpl *>(clis->clients[i])->w);
    if (subs)
      for (size_t i = 0; i < subs->subscriber_count; i++)
        fn(static_cast<SubscriptionImpl *>(subs->subscribers[i])->w);
    if (evs)
      for (size_t i = 0; i < evs->event_count; i++)
        if (EventState *es = event_state_of(evs->events[i])) fn(es->w);
  };
  // Register before checking, so a trigger between the check and the receive still wakes this wait.
  const int prio = assigned_priority();
  for_each([&](Waitable &w) { w.add(coid, prio); });

  auto ready = [&]() {
    bool any = false;
    if (gcs)
      for (size_t i = 0; i < gcs->guard_condition_count; i++)
        any |= static_cast<GuardCondition *>(gcs->guard_conditions[i])->triggered.load();
    if (srvs)
      for (size_t i = 0; i < srvs->service_count; i++) {
        auto *s = static_cast<ServiceImpl *>(srvs->services[i]);
        std::lock_guard<std::mutex> l(s->mu);
        any |= !s->queue.empty();
      }
    if (clis)
      for (size_t i = 0; i < clis->client_count; i++) {
        auto *c = static_cast<ClientImpl *>(clis->clients[i]);
        std::lock_guard<std::mutex> l(c->mu);
        any |= !c->responses.empty();
      }
    if (subs)
      for (size_t i = 0; i < subs->subscriber_count; i++)
        any |= static_cast<SubscriptionImpl *>(subs->subscribers[i])->has_data();
    if (evs)
      for (size_t i = 0; i < evs->event_count; i++)
        if (EventState *es = event_state_of(evs->events[i])) any |= es->ready();
    return any;
  };

  const bool forever = timeout == nullptr;
  const int64_t limit = forever ? 0 : int64_t(timeout->sec) * 1000000000 + int64_t(timeout->nsec);
  timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  const int64_t start = int64_t(t0.tv_sec) * 1000000000 + t0.tv_nsec;
  static thread_local std::vector<uint8_t> buf(64 * 1024);
  bool timed_out = false;
  bool received_once = false;
  while (!ready()) {
    uint64_t left = 0;
    if (!forever) {
      timespec t;
      clock_gettime(CLOCK_MONOTONIC, &t);
      const int64_t el = int64_t(t.tv_sec) * 1000000000 + t.tv_nsec - start;
      // Even a zero timeout (a poll, as spin_some does) receives once without blocking, so a request already
      // waiting on the channel is accepted.
      if (el >= limit && received_once) {
        timed_out = true;
        break;
      }
      left = el >= limit ? 0 : uint64_t(limit - el);
      TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, nullptr, &left, nullptr);
    }
    received_once = true;
    _msg_info info;
    const rcvid_t rcvid = MsgReceive(chid, buf.data(), buf.size(), &info);
    if (rcvid > 0) {
      const long got = info.msglen < long(buf.size()) ? info.msglen : long(buf.size());
      accept_request(ctx, rcvid, info, buf.data(), size_t(got), coid);
    }
    // A pulse (rcvid 0) only wakes; a timeout (ETIMEDOUT) is checked at the top.
  }
  for_each([&](Waitable &w) { w.remove(coid, prio); });

  if (gcs)
    for (size_t i = 0; i < gcs->guard_condition_count; i++) {
      auto *g = static_cast<GuardCondition *>(gcs->guard_conditions[i]);
      if (!g->triggered.exchange(false)) gcs->guard_conditions[i] = nullptr;
    }
  if (srvs)
    for (size_t i = 0; i < srvs->service_count; i++) {
      auto *s = static_cast<ServiceImpl *>(srvs->services[i]);
      std::lock_guard<std::mutex> l(s->mu);
      if (s->queue.empty()) srvs->services[i] = nullptr;
    }
  if (clis)
    for (size_t i = 0; i < clis->client_count; i++) {
      auto *c = static_cast<ClientImpl *>(clis->clients[i]);
      std::lock_guard<std::mutex> l(c->mu);
      if (c->responses.empty()) clis->clients[i] = nullptr;
    }
  if (subs)
    for (size_t i = 0; i < subs->subscriber_count; i++)
      if (!static_cast<SubscriptionImpl *>(subs->subscribers[i])->has_data()) subs->subscribers[i] = nullptr;
  if (evs)
    for (size_t i = 0; i < evs->event_count; i++)
    {
      EventState *es = event_state_of(evs->events[i]);
      if (!es || !es->ready()) evs->events[i] = nullptr;
    }
  return timed_out ? RMW_RET_TIMEOUT : RMW_RET_OK;
}

// ---- services ----

rmw_service_t *rmw_create_service(const rmw_node_t *node, const rosidl_service_type_support_t *ts,
                                  const char *name, const rmw_qos_profile_t *qos) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(node->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  if (!valid_name(name, qos)) return nullptr;
  bool ts_ok = false;
  const its::ServiceMembers *sm = service_members(ts, &ts_ok);
  if (!ts_ok) return nullptr;
  rmw_context_impl_s *ctx = node->context->impl;
  auto *s = new ServiceImpl();
  s->ctx = ctx;
  s->req = sm ? sm->request_members_ : nullptr;
  s->res = sm ? sm->response_members_ : nullptr;
  {
    std::lock_guard<std::mutex> l(ctx->mu);
    s->index = ctx->next_service++;
    ctx->services[s->index] = s;
  }
  s->reg_path = registry_path(ctx, name);
  const std::string tmp = s->reg_path + ".tmp" + std::to_string(getpid());
  {
    std::ofstream f(tmp);
    f << getpid() << " " << ctx->chid << " " << s->index << "\n";
  }
  rename(tmp.c_str(), s->reg_path.c_str());
  rmw_service_t *srv = rmw_service_allocate();
  srv->implementation_identifier = kId;
  srv->data = s;
  srv->service_name = rcutils_strdup(name, rcutils_get_default_allocator());
  uint8_t gid[RMW_GID_STORAGE_SIZE];
  make_gid(ctx, gid);
  s->graph_id = graph_add_endpoint(node, rmw_qnx::core::EndpointKind::kService, name, service_type_name(ts), gid, *qos);
  return srv;
}

rmw_ret_t rmw_destroy_service(rmw_node_t *node, rmw_service_t *srv) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(srv, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<ServiceImpl *>(srv->data);
  graph_remove(s->ctx, s->graph_id);
  {
    std::lock_guard<std::mutex> l(s->ctx->mu);
    s->ctx->services.erase(s->index);
  }
  pid_t pid;
  int chid;
  uint32_t index;
  if (read_registry(s->reg_path, &pid, &chid, &index) && pid == getpid() && index == s->index)
    unlink(s->reg_path.c_str());
  {
    std::lock_guard<std::mutex> l(s->mu);
    for (auto &p : s->queue) MsgError(p.rcvid, ESRCH);
    for (auto &kv : s->inflight) MsgError(kv.second, ESRCH);
  }
  delete s;
  rcutils_allocator_t a = rcutils_get_default_allocator();
  a.deallocate(const_cast<char *>(srv->service_name), a.state);
  rmw_service_free(srv);
  return RMW_RET_OK;
}

rmw_ret_t rmw_take_request(const rmw_service_t *srv, rmw_service_info_t *hdr, void *ros_request, bool *taken) {
  CHECK_ID(srv, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(hdr, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<ServiceImpl *>(srv->data);
  *taken = false;
  Pending p;
  {
    std::lock_guard<std::mutex> l(s->mu);
    if (s->queue.empty()) return RMW_RET_OK;
    p = std::move(s->queue.front());
    s->queue.pop_front();
  }
  if (!deserialize_safe(s->req, ros_request, p.data.data(), p.data.size())) {
    MsgError(p.rcvid, EBADMSG);
    RMW_SET_ERROR_MSG("rmw_qnx: malformed request");
    return RMW_RET_ERROR;
  }
  std::memcpy(hdr->request_id.writer_guid, p.guid, RMW_GID_STORAGE_SIZE);
  hdr->request_id.sequence_number = p.seq;
  hdr->source_timestamp = p.received_ns;
  hdr->received_timestamp = p.received_ns;
  {
    std::lock_guard<std::mutex> l(s->mu);
    s->inflight[{std::string(reinterpret_cast<char *>(p.guid), RMW_GID_STORAGE_SIZE), p.seq}] = p.rcvid;
  }
  *taken = true;
  return RMW_RET_OK;
}

rmw_ret_t rmw_send_response(const rmw_service_t *srv, rmw_request_id_t *id, void *ros_response) {
  CHECK_ID(srv, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(id, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<ServiceImpl *>(srv->data);
  rcvid_t rcvid;
  {
    std::lock_guard<std::mutex> l(s->mu);
    auto it = s->inflight.find({std::string(reinterpret_cast<char *>(id->writer_guid), RMW_GID_STORAGE_SIZE),
                                id->sequence_number});
    if (it == s->inflight.end()) {
      RMW_SET_ERROR_MSG("rmw_qnx: no request with this id is waiting for a response");
      return RMW_RET_ERROR;
    }
    rcvid = it->second;
    s->inflight.erase(it);
  }
  std::vector<uint8_t> body;
  CdrWriter w(&body);
  if (!s->res || !rmw_qnx::serialize_cpp(s->res, ros_response, w) || body.size() > kMaxReply) {
    MsgError(rcvid, EMSGSIZE);
    RMW_SET_ERROR_MSG("rmw_qnx: response could not be serialised or is larger than 64 KiB");
    return RMW_RET_ERROR;
  }
  RepHdr h{kRepMagic, 0, id->sequence_number, uint32_t(body.size()), 0};
  iov_t iov[2];
  SETIOV(&iov[0], &h, sizeof h);
  SETIOV(&iov[1], body.data(), body.size());
  // The client may have gone: the reply then fails, which is not this server's error.
  MsgReplyv(rcvid, EOK, iov, 2);
  return RMW_RET_OK;
}

rmw_ret_t rmw_service_request_subscription_get_actual_qos(const rmw_service_t *srv, rmw_qos_profile_t *qos) {
  CHECK_ID(srv, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  *qos = rmw_qos_profile_services_default;
  return RMW_RET_OK;
}

rmw_ret_t rmw_service_response_publisher_get_actual_qos(const rmw_service_t *srv, rmw_qos_profile_t *qos) {
  return rmw_service_request_subscription_get_actual_qos(srv, qos);
}

// ---- clients ----

rmw_client_t *rmw_create_client(const rmw_node_t *node, const rosidl_service_type_support_t *ts, const char *name,
                                const rmw_qos_profile_t *qos) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(node->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  if (!valid_name(name, qos)) return nullptr;
  bool ts_ok = false;
  const its::ServiceMembers *sm = service_members(ts, &ts_ok);
  if (!ts_ok) return nullptr;
  auto *c = new ClientImpl();
  c->ctx = node->context->impl;
  c->req = sm ? sm->request_members_ : nullptr;
  c->res = sm ? sm->response_members_ : nullptr;
  c->reg_path = registry_path(c->ctx, name);
  make_gid(c->ctx, c->gid);
  c->chid = ChannelCreate(_NTO_CHF_PRIVATE);
  c->coid = ConnectAttach(0, 0, c->chid, _NTO_SIDE_CHANNEL, 0);
  c->sender = std::thread(sender_main, c);
  rmw_client_t *cli = rmw_client_allocate();
  cli->implementation_identifier = kId;
  cli->data = c;
  cli->service_name = rcutils_strdup(name, rcutils_get_default_allocator());
  c->graph_id = graph_add_endpoint(node, rmw_qnx::core::EndpointKind::kClient, name, service_type_name(ts), c->gid, *qos);
  return cli;
}

rmw_ret_t rmw_destroy_client(rmw_node_t *node, rmw_client_t *cli) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(cli, RMW_RET_INVALID_ARGUMENT);
  auto *c = static_cast<ClientImpl *>(cli->data);
  graph_remove(c->ctx, c->graph_id);
  MsgSendPulse(c->coid, -1, kPulseQuit, 0);
  c->sender.join();
  if (c->srv_coid != -1) ConnectDetach(c->srv_coid);
  ConnectDetach(c->coid);
  ChannelDestroy(c->chid);
  delete c;
  rcutils_allocator_t a = rcutils_get_default_allocator();
  a.deallocate(const_cast<char *>(cli->service_name), a.state);
  rmw_client_free(cli);
  return RMW_RET_OK;
}

rmw_ret_t rmw_send_request(const rmw_client_t *cli, const void *ros_request, int64_t *seq) {
  CHECK_ID(cli, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(seq, RMW_RET_INVALID_ARGUMENT);
  auto *c = static_cast<ClientImpl *>(cli->data);
  std::vector<uint8_t> body;
  CdrWriter w(&body);
  if (!c->req || !rmw_qnx::serialize_cpp(c->req, ros_request, w)) {
    RMW_SET_ERROR_MSG("rmw_qnx: request could not be serialised");
    return RMW_RET_ERROR;
  }
  *seq = c->next_seq.fetch_add(1);
  sched_param sp{};
  SchedGet(0, 0, &sp);
  const int prio = sp.sched_curpriority;
  {
    std::lock_guard<std::mutex> l(c->mu);
    c->to_send.push(rmw_qnx::core::Outgoing{*seq, prio, std::move(body)});
  }
  // The pulse runs the sender thread at this thread's priority and names it, so the sender takes this request.
  if (MsgSendPulse(c->coid, prio, kPulseSend, prio) == -1) {
    RMW_SET_ERROR_MSG("rmw_qnx: MsgSendPulse failed");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

rmw_ret_t rmw_take_response(const rmw_client_t *cli, rmw_service_info_t *hdr, void *ros_response, bool *taken) {
  CHECK_ID(cli, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(hdr, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  auto *c = static_cast<ClientImpl *>(cli->data);
  *taken = false;
  Response resp;
  {
    std::lock_guard<std::mutex> l(c->mu);
    if (c->responses.empty()) return RMW_RET_OK;
    resp = std::move(c->responses.front());
    c->responses.pop_front();
  }
  if (!deserialize_safe(c->res, ros_response, resp.data.data(), resp.data.size())) {
    RMW_SET_ERROR_MSG("rmw_qnx: malformed response");
    return RMW_RET_ERROR;
  }
  std::memcpy(hdr->request_id.writer_guid, c->gid, RMW_GID_STORAGE_SIZE);
  hdr->request_id.sequence_number = resp.seq;
  hdr->source_timestamp = resp.received_ns;
  hdr->received_timestamp = resp.received_ns;
  *taken = true;
  return RMW_RET_OK;
}

rmw_ret_t rmw_client_request_publisher_get_actual_qos(const rmw_client_t *cli, rmw_qos_profile_t *qos) {
  CHECK_ID(cli, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  *qos = rmw_qos_profile_services_default;
  return RMW_RET_OK;
}

rmw_ret_t rmw_client_response_subscription_get_actual_qos(const rmw_client_t *cli, rmw_qos_profile_t *qos) {
  return rmw_client_request_publisher_get_actual_qos(cli, qos);
}

rmw_ret_t rmw_service_server_is_available(const rmw_node_t *node, const rmw_client_t *cli, bool *available) {
  CHECK_ID(node, RMW_RET_ERROR);
  CHECK_ID(cli, RMW_RET_ERROR);
  RMW_CHECK_ARGUMENT_FOR_NULL(available, RMW_RET_ERROR);
  auto *c = static_cast<ClientImpl *>(cli->data);
  pid_t pid;
  int chid;
  uint32_t index;
  *available = read_registry(c->reg_path, &pid, &chid, &index);
  return RMW_RET_OK;
}

rmw_ret_t rmw_get_gid_for_client(const rmw_client_t *cli, rmw_gid_t *gid) {
  CHECK_ID(cli, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
  gid->implementation_identifier = kId;
  std::memcpy(gid->data, static_cast<ClientImpl *>(cli->data)->gid, RMW_GID_STORAGE_SIZE);
  return RMW_RET_OK;
}

rmw_ret_t rmw_compare_gids_equal(const rmw_gid_t *a, const rmw_gid_t *b, bool *result) {
  CHECK_ID(a, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(b, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(result, RMW_RET_INVALID_ARGUMENT);
  *result = std::memcmp(a->data, b->data, RMW_GID_STORAGE_SIZE) == 0;
  return RMW_RET_OK;
}

rmw_ret_t rmw_service_set_on_new_request_callback(rmw_service_t *, rmw_event_callback_t, const void *) {
  return unsupported("rmw_service_set_on_new_request_callback");
}
rmw_ret_t rmw_client_set_on_new_response_callback(rmw_client_t *, rmw_event_callback_t, const void *) {
  return unsupported("rmw_client_set_on_new_response_callback");
}

// ---- QoS ----

rmw_ret_t rmw_qos_profile_check_compatible(const rmw_qos_profile_t pub, const rmw_qos_profile_t sub,
                                           rmw_qos_compatibility_type_t *compat, char *reason, size_t reason_size) {
  RMW_CHECK_ARGUMENT_FOR_NULL(compat, RMW_RET_INVALID_ARGUMENT);
  if (!reason && reason_size) return RMW_RET_INVALID_ARGUMENT;
  const rmw_qnx::core::CompatResult r = rmw_qnx::core::check_compatible(to_core(pub), to_core(sub));
  *compat = r.level == rmw_qnx::core::Compat::kOk        ? RMW_QOS_COMPATIBILITY_OK
            : r.level == rmw_qnx::core::Compat::kWarning ? RMW_QOS_COMPATIBILITY_WARNING
                                                         : RMW_QOS_COMPATIBILITY_ERROR;
  if (reason_size) std::snprintf(reason, reason_size, "%s", r.reason.c_str());
  return RMW_RET_OK;
}

// ---- not yet supported: topics, graph queries, events, loans, dynamic types ----

rmw_ret_t rmw_init_publisher_allocation(const rosidl_message_type_support_t *,
                                        const rosidl_runtime_c__Sequence__bound *, rmw_publisher_allocation_t *) {
  return unsupported("publisher allocation");
}
rmw_ret_t rmw_fini_publisher_allocation(rmw_publisher_allocation_t *) { return unsupported("publisher allocation"); }
// ---- topics ----
//
// A publisher serialises once into the topic's ring and pulses each subscriber at the priority it registered with
// (topics.hpp). The subscriber's context relay thread takes that pulse at that priority and wakes the wait set.
// Types offered only with the C type support (rcl's /rosout) are created and counted, but carry no data yet.

static rmw_qos_profile_t resolve(const rmw_qos_profile_t &q) {
  rmw_qos_profile_t r = q;
  if (r.history == RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT) r.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  if (r.depth == 0 && r.history == RMW_QOS_POLICY_HISTORY_KEEP_LAST) r.depth = 10;
  if (r.reliability == RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT) r.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
  if (r.durability == RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT) r.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  if (r.liveliness == RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT) r.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
  return r;
}

struct PublisherImpl {
  std::unique_ptr<rmw_qnx::Topic> topic;
  const its::MessageMembers *members = nullptr;
  rmw_qos_profile_t qos{};
  uint8_t gid[RMW_GID_STORAGE_SIZE] = {};
  std::atomic<uint64_t> seq{1};
  rmw_context_impl_s *ctx = nullptr;
  uint64_t graph_id = 0;
  EventState deadline_ev, matched_ev, inert_ev;  // inert: a created event of a type that is never raised
};

// The C++ introspection members of a message type, or nullptr if it has none.
static const its::MessageMembers *message_members(const rosidl_message_type_support_t *ts, bool *ok) {
  const rosidl_message_type_support_t *h = get_message_typesupport_handle(ts, its::typesupport_identifier);
  rmw_reset_error();
  *ok = true;
  if (h) return static_cast<const its::MessageMembers *>(h->data);
  *ok = get_message_typesupport_handle(ts, "rosidl_typesupport_introspection_c") != nullptr;
  rmw_reset_error();
  if (!*ok) RMW_SET_ERROR_MSG("rmw_qnx: this message type has no introspection type support");
  return nullptr;
}

static std::string type_name(const its::MessageMembers *m) {
  std::string ns = m->message_namespace_;
  for (size_t i; (i = ns.find("::")) != std::string::npos;) ns.replace(i, 2, "/");
  return ns + "/" + m->message_name_;
}

static std::unique_ptr<rmw_qnx::Topic> open_topic(rmw_context_impl_s *ctx, const char *name,
                                           const its::MessageMembers *m) {
  if (!m) return nullptr;
  auto t = std::make_unique<rmw_qnx::Topic>(ctx->domain, name, type_name(m));
  if (!t->error().empty()) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_qnx: %s", t->error().c_str());
    return nullptr;
  }
  return t;
}

rmw_publisher_t *rmw_create_publisher(const rmw_node_t *node, const rosidl_message_type_support_t *ts,
                                      const char *name, const rmw_qos_profile_t *qos,
                                      const rmw_publisher_options_t *opts) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(node->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(opts, nullptr);
  if (!valid_name(name, qos)) return nullptr;
  bool ts_ok = false;
  auto e = std::make_unique<PublisherImpl>();
  e->members = message_members(ts, &ts_ok);
  if (!ts_ok) return nullptr;
  e->topic = open_topic(node->context->impl, name, e->members);
  if (e->members && !e->topic) return nullptr;
  if (e->topic) e->topic->add_publisher();
  e->qos = resolve(*qos);
  make_gid(node->context->impl, e->gid);
  e->ctx = node->context->impl;
  e->graph_id = graph_add_endpoint(node, rmw_qnx::core::EndpointKind::kPublisher, name, message_type_name(ts), e->gid, e->qos);
  rmw_publisher_t *p = rmw_publisher_allocate();
  p->implementation_identifier = kId;
  p->data = e.release();
  p->topic_name = rcutils_strdup(name, rcutils_get_default_allocator());
  p->options = *opts;
  p->can_loan_messages = false;
  return p;
}
rmw_ret_t rmw_destroy_publisher(rmw_node_t *node, rmw_publisher_t *p) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  auto *e = static_cast<PublisherImpl *>(p->data);
  event_teardown(e->ctx, &e->deadline_ev);
  event_teardown(e->ctx, &e->matched_ev);
  if (e->topic) e->topic->remove_publisher();
  graph_remove(e->ctx, e->graph_id);
  delete e;
  rcutils_allocator_t a = rcutils_get_default_allocator();
  a.deallocate(const_cast<char *>(p->topic_name), a.state);
  rmw_publisher_free(p);
  return RMW_RET_OK;
}
rmw_ret_t rmw_borrow_loaned_message(const rmw_publisher_t *, const rosidl_message_type_support_t *, void **) {
  return unsupported("loaned messages");
}
rmw_ret_t rmw_return_loaned_message_from_publisher(const rmw_publisher_t *, void *) {
  return unsupported("loaned messages");
}
static rmw_ret_t publish_cdr(PublisherImpl *e, const uint8_t *cdr, size_t len) {
  if (!e->topic) return RMW_RET_OK;  // C type support only: not carried yet
  e->deadline_ev.touch();
  if (!e->topic->publish(e->gid, e->seq.fetch_add(1), cdr, len, kPulseTopic)) {
    RMW_SET_ERROR_MSG("rmw_qnx: message larger than a topic slot (16 KiB)");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}
rmw_ret_t rmw_publish(const rmw_publisher_t *p, const void *msg, rmw_publisher_allocation_t *) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(msg, RMW_RET_INVALID_ARGUMENT);
  auto *e = static_cast<PublisherImpl *>(p->data);
  if (!e->topic) return RMW_RET_OK;
  static thread_local std::vector<uint8_t> body;
  CdrWriter w(&body);
  if (!rmw_qnx::serialize_cpp(e->members, msg, w)) {
    RMW_SET_ERROR_MSG("rmw_qnx: message could not be serialised");
    return RMW_RET_ERROR;
  }
  return publish_cdr(e, body.data(), body.size());
}
rmw_ret_t rmw_publish_serialized_message(const rmw_publisher_t *p, const rmw_serialized_message_t *m,
                                         rmw_publisher_allocation_t *) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(m, RMW_RET_INVALID_ARGUMENT);
  if (!m->buffer && m->buffer_length) {
    RMW_SET_ERROR_MSG("rmw_qnx: serialized message has a length but no buffer");
    return RMW_RET_INVALID_ARGUMENT;
  }
  return publish_cdr(static_cast<PublisherImpl *>(p->data), m->buffer, m->buffer_length);
}
rmw_ret_t rmw_publish_loaned_message(const rmw_publisher_t *, void *, rmw_publisher_allocation_t *) {
  return unsupported("loaned messages");
}
rmw_ret_t rmw_publisher_count_matched_subscriptions(const rmw_publisher_t *p, size_t *n) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(n, RMW_RET_INVALID_ARGUMENT);
  auto *e = static_cast<PublisherImpl *>(p->data);
  *n = e->topic ? e->topic->subscribers() : 0;
  return RMW_RET_OK;
}
rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t *p, rmw_qos_profile_t *qos) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  *qos = static_cast<PublisherImpl *>(p->data)->qos;
  return RMW_RET_OK;
}
rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t *p, rmw_gid_t *gid) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
  gid->implementation_identifier = kId;
  std::memcpy(gid->data, static_cast<PublisherImpl *>(p->data)->gid, RMW_GID_STORAGE_SIZE);
  return RMW_RET_OK;
}

rmw_subscription_t *rmw_create_subscription(const rmw_node_t *node, const rosidl_message_type_support_t *ts,
                                            const char *name, const rmw_qos_profile_t *qos,
                                            const rmw_subscription_options_t *opts) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(node->implementation_identifier, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(opts, nullptr);
  rmw_context_impl_s *ctx = node->context->impl;
  if (!valid_name(name, qos)) return nullptr;
  bool ts_ok = false;
  const its::MessageMembers *members = message_members(ts, &ts_ok);
  if (!ts_ok) return nullptr;
  auto *s = new SubscriptionImpl();
  s->ctx = ctx;
  s->topic_name = name;
  s->members = members;
  s->topic = open_topic(ctx, name, s->members);
  if (s->members && !s->topic) {
    delete s;
    return nullptr;
  }
  s->qos = resolve(*qos);
  make_gid(ctx, s->gid);
  {
    std::lock_guard<std::mutex> l(ctx->mu);
    s->id = ctx->next_sub++;
    ctx->subs[s->id] = s;
  }
  if (s->topic) {
    s->pos = s->qos.durability == RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL
                 ? s->topic->ring().reader_start_with_history(uint32_t(s->qos.depth))
                 : s->topic->ring().reader_start_now();
    // Woken at the priority of the thread that creates the subscription.
    s->slot = s->topic->add_subscriber(ctx->topic_chid, s->id, assigned_priority());
    if (s->slot < 0) {
      // A subscription the publishers cannot pulse would never wake: refuse it and undo the registration.
      {
        std::lock_guard<std::mutex> l(ctx->mu);
        ctx->subs.erase(s->id);
      }
      delete s;
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_qnx: topic %s already has %d subscriptions, the most it holds", name,
                                           rmw_qnx::kMaxSubs);
      return nullptr;
    }
  }
  s->graph_id = graph_add_endpoint(node, rmw_qnx::core::EndpointKind::kSubscription, name, message_type_name(ts), s->gid, s->qos);
  rmw_subscription_t *sub = rmw_subscription_allocate();
  sub->implementation_identifier = kId;
  sub->data = s;
  sub->topic_name = rcutils_strdup(name, rcutils_get_default_allocator());
  sub->options = *opts;
  sub->can_loan_messages = false;
  sub->is_cft_enabled = false;
  return sub;
}
rmw_ret_t rmw_destroy_subscription(rmw_node_t *node, rmw_subscription_t *sub) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(sub, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<SubscriptionImpl *>(sub->data);
  event_teardown(s->ctx, &s->matched_ev);
  if (s->topic) s->topic->remove_subscriber(s->slot);
  graph_remove(s->ctx, s->graph_id);
  {
    std::lock_guard<std::mutex> l(s->ctx->mu);
    s->ctx->subs.erase(s->id);
  }
  delete s;
  rcutils_allocator_t a = rcutils_get_default_allocator();
  a.deallocate(const_cast<char *>(sub->topic_name), a.state);
  rmw_subscription_free(sub);
  return RMW_RET_OK;
}
rmw_ret_t rmw_subscription_count_matched_publishers(const rmw_subscription_t *sub, size_t *n) {
  CHECK_ID(sub, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(n, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<SubscriptionImpl *>(sub->data);
  *n = s->topic ? s->topic->publishers() : 0;
  return RMW_RET_OK;
}
rmw_ret_t rmw_subscription_get_actual_qos(const rmw_subscription_t *sub, rmw_qos_profile_t *qos) {
  CHECK_ID(sub, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  *qos = static_cast<SubscriptionImpl *>(sub->data)->qos;
  return RMW_RET_OK;
}

// Takes the next message's bytes (after the prefix) into *out. KEEP_LAST: a backlog deeper than the depth is
// skipped to the last `depth` messages.
static bool take_bytes(SubscriptionImpl *s, std::vector<uint8_t> *out, rmw_message_info_t *info) {
  if (!s->topic) return false;
  rmw_qnx::core::Ring &r = s->topic->ring();
  std::lock_guard<std::mutex> l(s->mu);
  const uint64_t head = r.head();
  if (s->qos.history == RMW_QOS_POLICY_HISTORY_KEEP_LAST && s->qos.depth > 0 && head > s->pos + s->qos.depth)
    s->pos = head - s->qos.depth;
  out->resize(r.payload());
  for (int tries = 0; tries < 1000; tries++) {
    rmw_qnx::core::TakeResult t = r.take(&s->pos, out->data(), uint32_t(out->size()));
    if (t.status == rmw_qnx::core::TakeStatus::kEmpty) return false;
    if (t.status != rmw_qnx::core::TakeStatus::kOk || t.length < rmw_qnx::kPrefix) continue;
    if (info) {
      *info = rmw_get_zero_initialized_message_info();
      info->source_timestamp = int64_t(t.stamp_ns);
      info->received_timestamp = now_ns();
      std::memcpy(&info->publication_sequence_number, out->data() + 16, 8);
      info->reception_sequence_number = int64_t(t.seq);
      info->publisher_gid.implementation_identifier = kId;
      std::memcpy(info->publisher_gid.data, out->data(), RMW_GID_STORAGE_SIZE);
      info->from_intra_process = false;
    }
    out->erase(out->begin(), out->begin() + rmw_qnx::kPrefix);
    out->resize(t.length - rmw_qnx::kPrefix);
    return true;
  }
  return false;
}

static rmw_ret_t take_into(const rmw_subscription_t *sub, void *msg, bool *taken, rmw_message_info_t *info) {
  CHECK_ID(sub, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(msg, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<SubscriptionImpl *>(sub->data);
  *taken = false;
  static thread_local std::vector<uint8_t> bytes;
  if (!take_bytes(s, &bytes, info)) return RMW_RET_OK;
  if (!deserialize_safe(s->members, msg, bytes.data(), bytes.size())) {
    RMW_SET_ERROR_MSG("rmw_qnx: malformed message");
    return RMW_RET_ERROR;
  }
  *taken = true;
  return RMW_RET_OK;
}

static rmw_ret_t take_serialized(const rmw_subscription_t *sub, rmw_serialized_message_t *m, bool *taken,
                          rmw_message_info_t *info) {
  CHECK_ID(sub, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(m, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  *taken = false;
  static thread_local std::vector<uint8_t> bytes;
  if (!take_bytes(static_cast<SubscriptionImpl *>(sub->data), &bytes, info)) return RMW_RET_OK;
  if (m->buffer_capacity < bytes.size() && rmw_serialized_message_resize(m, bytes.size()) != RMW_RET_OK)
    return RMW_RET_BAD_ALLOC;
  std::memcpy(m->buffer, bytes.data(), bytes.size());
  m->buffer_length = bytes.size();
  *taken = true;
  return RMW_RET_OK;
}
rmw_ret_t rmw_publisher_event_init(rmw_event_t *ev, const rmw_publisher_t *e, rmw_event_type_t type) {
  RMW_CHECK_ARGUMENT_FOR_NULL(ev, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(e, RMW_RET_INVALID_ARGUMENT);
  auto *p = static_cast<PublisherImpl *>(e->data);
  EventState *es = nullptr;
  if (type == RMW_EVENT_OFFERED_DEADLINE_MISSED) {
    es = &p->deadline_ev;
    event_setup(p->ctx, es, EventState::kDeadline, nullptr, false, to_core(p->qos).deadline_ns);
  } else if (type == RMW_EVENT_PUBLICATION_MATCHED && p->topic) {
    es = &p->matched_ev;
    event_setup(p->ctx, es, EventState::kMatched, p->topic.get(), true, 0);
  } else {
    es = &p->inert_ev;  // created, never raised
  }
  ev->implementation_identifier = kId;
  ev->data = es;
  ev->event_type = type;
  return RMW_RET_OK;
}
rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t *) { return unsupported("liveliness"); }
rmw_ret_t rmw_publisher_wait_for_all_acked(const rmw_publisher_t *, rmw_time_t) { return unsupported("topics"); }
rmw_ret_t rmw_get_serialized_message_size(const rosidl_message_type_support_t *,
                                          const rosidl_runtime_c__Sequence__bound *, size_t *) {
  return unsupported("rmw_get_serialized_message_size");
}
rmw_ret_t rmw_serialize(const void *msg, const rosidl_message_type_support_t *ts, rmw_serialized_message_t *out) {
  RMW_CHECK_ARGUMENT_FOR_NULL(msg, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(out, RMW_RET_INVALID_ARGUMENT);
  bool ok = false;
  const its::MessageMembers *m = message_members(ts, &ok);
  if (!m) {
    if (ok) RMW_SET_ERROR_MSG("rmw_qnx: serialising C type support messages is not supported yet");
    return RMW_RET_ERROR;
  }
  std::vector<uint8_t> body;
  CdrWriter w(&body);
  if (!rmw_qnx::serialize_cpp(m, msg, w)) return RMW_RET_ERROR;
  if (out->buffer_capacity < body.size() && rmw_serialized_message_resize(out, body.size()) != RMW_RET_OK)
    return RMW_RET_BAD_ALLOC;
  std::memcpy(out->buffer, body.data(), body.size());
  out->buffer_length = body.size();
  return RMW_RET_OK;
}
rmw_ret_t rmw_deserialize(const rmw_serialized_message_t *in, const rosidl_message_type_support_t *ts, void *msg) {
  RMW_CHECK_ARGUMENT_FOR_NULL(in, RMW_RET_INVALID_ARGUMENT);
  if (!in->buffer && in->buffer_length) {
    RMW_SET_ERROR_MSG("rmw_qnx: serialized message has a length but no buffer");
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(msg, RMW_RET_INVALID_ARGUMENT);
  bool ok = false;
  const its::MessageMembers *m = message_members(ts, &ok);
  if (!m) {
    if (ok) RMW_SET_ERROR_MSG("rmw_qnx: deserialising C type support messages is not supported yet");
    return RMW_RET_ERROR;
  }
  if (!deserialize_safe(m, msg, in->buffer, in->buffer_length)) {
    RMW_SET_ERROR_MSG("rmw_qnx: malformed serialized message");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}
rmw_ret_t rmw_init_subscription_allocation(const rosidl_message_type_support_t *,
                                           const rosidl_runtime_c__Sequence__bound *, rmw_subscription_allocation_t *) {
  return unsupported("subscription allocation");
}
rmw_ret_t rmw_fini_subscription_allocation(rmw_subscription_allocation_t *) {
  return unsupported("subscription allocation");
}
rmw_ret_t rmw_subscription_event_init(rmw_event_t *ev, const rmw_subscription_t *e, rmw_event_type_t type) {
  RMW_CHECK_ARGUMENT_FOR_NULL(ev, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(e, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<SubscriptionImpl *>(e->data);
  EventState *es = &s->inert_ev;  // created, never raised, unless supported below
  if (type == RMW_EVENT_SUBSCRIPTION_MATCHED && s->topic) {
    es = &s->matched_ev;
    event_setup(s->ctx, es, EventState::kMatched, s->topic.get(), false, 0);
  }
  ev->implementation_identifier = kId;
  ev->data = es;
  ev->event_type = type;
  return RMW_RET_OK;
}
rmw_ret_t rmw_subscription_set_content_filter(rmw_subscription_t *, const rmw_subscription_content_filter_options_t *) {
  return unsupported("content filters");
}
rmw_ret_t rmw_subscription_get_content_filter(const rmw_subscription_t *, rcutils_allocator_t *,
                                              rmw_subscription_content_filter_options_t *) {
  return unsupported("content filters");
}
rmw_ret_t rmw_take(const rmw_subscription_t *sub, void *msg, bool *taken, rmw_subscription_allocation_t *) {
  return take_into(sub, msg, taken, nullptr);
}
rmw_ret_t rmw_take_with_info(const rmw_subscription_t *sub, void *msg, bool *taken, rmw_message_info_t *info,
                             rmw_subscription_allocation_t *) {
  RMW_CHECK_ARGUMENT_FOR_NULL(info, RMW_RET_INVALID_ARGUMENT);
  return take_into(sub, msg, taken, info);
}
rmw_ret_t rmw_take_serialized_message(const rmw_subscription_t *sub, rmw_serialized_message_t *m, bool *taken,
                                      rmw_subscription_allocation_t *) {
  return take_serialized(sub, m, taken, nullptr);
}
rmw_ret_t rmw_take_serialized_message_with_info(const rmw_subscription_t *sub, rmw_serialized_message_t *m,
                                                bool *taken, rmw_message_info_t *info,
                                                rmw_subscription_allocation_t *) {
  RMW_CHECK_ARGUMENT_FOR_NULL(info, RMW_RET_INVALID_ARGUMENT);
  return take_serialized(sub, m, taken, info);
}
rmw_ret_t rmw_take_sequence(const rmw_subscription_t *, size_t, rmw_message_sequence_t *,
                            rmw_message_info_sequence_t *, size_t *, rmw_subscription_allocation_t *) {
  return unsupported("topics");
}
rmw_ret_t rmw_take_loaned_message(const rmw_subscription_t *, void **, bool *, rmw_subscription_allocation_t *) {
  return unsupported("loaned messages");
}
rmw_ret_t rmw_take_loaned_message_with_info(const rmw_subscription_t *, void **, bool *, rmw_message_info_t *,
                                            rmw_subscription_allocation_t *) {
  return unsupported("loaned messages");
}
rmw_ret_t rmw_return_loaned_message_from_subscription(const rmw_subscription_t *, void *) {
  return unsupported("loaned messages");
}
rmw_ret_t rmw_take_event(const rmw_event_t *ev, void *info, bool *taken) {
  CHECK_ID(ev, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(info, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  *taken = false;
  auto *es = static_cast<EventState *>(ev->data);
  std::lock_guard<std::mutex> l(es->mu);
  if (es->kind == EventState::kDeadline) {
    const int64_t cur = es->missed_locked(mono_ns());
    if (cur <= es->reported) return RMW_RET_OK;
    auto *st = static_cast<rmw_offered_deadline_missed_status_t *>(info);
    st->total_count = int32_t(cur);
    st->total_count_change = int32_t(cur - es->reported);
    es->reported = cur;
    *taken = true;
  } else if (es->kind == EventState::kMatched) {
    const uint32_t joins = es->joins();
    const int64_t cur = es->current();
    if (joins == es->joins_seen && cur == es->reported_current) return RMW_RET_OK;
    auto *st = static_cast<rmw_matched_status_t *>(info);
    es->total += int64_t(uint32_t(joins - es->joins_seen));
    st->total_count = size_t(es->total);
    st->total_count_change = int32_t(joins - es->joins_seen);
    st->current_count = size_t(cur);
    st->current_count_change = int32_t(cur - es->reported_current);
    es->joins_seen = joins;
    es->reported_current = cur;
    *taken = true;
  }
  return RMW_RET_OK;
}
// Deadline missed (offered) and matched events are raised. Requested-deadline, liveliness and incompatible-QoS
// events are not.
bool rmw_event_type_is_supported(rmw_event_type_t t) {
  return t == RMW_EVENT_OFFERED_DEADLINE_MISSED || t == RMW_EVENT_PUBLICATION_MATCHED ||
         t == RMW_EVENT_SUBSCRIPTION_MATCHED;
}
// ---- graph queries: answered from every live context's graph file ----

static rmw_ret_t check_query_node(const rmw_node_t *node) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(node->implementation_identifier, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId,
                                   return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  return RMW_RET_OK;
}

static rmw_ret_t check_allocator(rcutils_allocator_t *a) {
  RMW_CHECK_ARGUMENT_FOR_NULL(a, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(a, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  return RMW_RET_OK;
}

static rmw_ret_t fill_names_and_types(const std::map<std::string, std::set<std::string>> &m, rcutils_allocator_t *a,
                                      rmw_names_and_types_t *out) {
  if (m.empty()) return RMW_RET_OK;
  rmw_ret_t r = rmw_names_and_types_init(out, m.size(), a);
  if (r != RMW_RET_OK) return r;
  size_t i = 0;
  for (const auto &kv : m) {
    out->names.data[i] = rcutils_strdup(kv.first.c_str(), *a);
    if (!out->names.data[i] || rcutils_string_array_init(&out->types[i], kv.second.size(), a) != RCUTILS_RET_OK) {
      rmw_names_and_types_fini(out);
      RMW_SET_ERROR_MSG("rmw_qnx: out of memory");
      return RMW_RET_BAD_ALLOC;
    }
    size_t j = 0;
    for (const auto &t : kv.second) {
      out->types[i].data[j] = rcutils_strdup(t.c_str(), *a);
      if (!out->types[i].data[j++]) {
        rmw_names_and_types_fini(out);
        RMW_SET_ERROR_MSG("rmw_qnx: out of memory");
        return RMW_RET_BAD_ALLOC;
      }
    }
    i++;
  }
  return RMW_RET_OK;
}

static rmw_ret_t names_and_types_by_node(const rmw_node_t *node, rcutils_allocator_t *a, const char *node_name,
                                         const char *node_ns, rmw_names_and_types_t *out,
                                         rmw_qnx::core::EndpointKind kind) {
  rmw_ret_t r = check_query_node(node);
  if (r != RMW_RET_OK) return r;
  if ((r = check_allocator(a)) != RMW_RET_OK) return r;
  RMW_CHECK_ARGUMENT_FOR_NULL(node_name, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(node_ns, RMW_RET_INVALID_ARGUMENT);
  int v = 0;
  if (rmw_validate_node_name(node_name, &v, nullptr) != RMW_RET_OK) return RMW_RET_ERROR;
  if (v != RMW_NODE_NAME_VALID) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node name: %s", rmw_node_name_validation_result_string(v));
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (rmw_validate_namespace(node_ns, &v, nullptr) != RMW_RET_OK) return RMW_RET_ERROR;
  if (v != RMW_NAMESPACE_VALID) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node namespace: %s", rmw_namespace_validation_result_string(v));
    return RMW_RET_INVALID_ARGUMENT;
  }
  if ((r = rmw_names_and_types_check_zero(out)) != RMW_RET_OK) return r;
  const rmw_qnx::core::Graph g = load_graph(node->context->impl->domain);
  if (!g.node_exists(node_name, node_ns)) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("node %s in namespace %s does not exist", node_name, node_ns);
    return RMW_RET_NODE_NAME_NON_EXISTENT;
  }
  const std::string n = node_name, ns = node_ns;
  return fill_names_and_types(g.names_and_types({kind}, &n, &ns), a, out);
}

rmw_ret_t rmw_get_publisher_names_and_types_by_node(const rmw_node_t *node, rcutils_allocator_t *a,
                                                    const char *node_name, const char *node_ns, bool,
                                                    rmw_names_and_types_t *out) {
  return names_and_types_by_node(node, a, node_name, node_ns, out, rmw_qnx::core::EndpointKind::kPublisher);
}
rmw_ret_t rmw_get_subscriber_names_and_types_by_node(const rmw_node_t *node, rcutils_allocator_t *a,
                                                     const char *node_name, const char *node_ns, bool,
                                                     rmw_names_and_types_t *out) {
  return names_and_types_by_node(node, a, node_name, node_ns, out, rmw_qnx::core::EndpointKind::kSubscription);
}
rmw_ret_t rmw_get_service_names_and_types_by_node(const rmw_node_t *node, rcutils_allocator_t *a,
                                                  const char *node_name, const char *node_ns,
                                                  rmw_names_and_types_t *out) {
  return names_and_types_by_node(node, a, node_name, node_ns, out, rmw_qnx::core::EndpointKind::kService);
}
rmw_ret_t rmw_get_client_names_and_types_by_node(const rmw_node_t *node, rcutils_allocator_t *a,
                                                 const char *node_name, const char *node_ns,
                                                 rmw_names_and_types_t *out) {
  return names_and_types_by_node(node, a, node_name, node_ns, out, rmw_qnx::core::EndpointKind::kClient);
}

static rmw_ret_t all_names_and_types(const rmw_node_t *node, rcutils_allocator_t *a, rmw_names_and_types_t *out,
                                     std::set<rmw_qnx::core::EndpointKind> kinds) {
  rmw_ret_t r = check_query_node(node);
  if (r != RMW_RET_OK) return r;
  if ((r = check_allocator(a)) != RMW_RET_OK) return r;
  if ((r = rmw_names_and_types_check_zero(out)) != RMW_RET_OK) return r;
  return fill_names_and_types(load_graph(node->context->impl->domain).names_and_types(kinds), a, out);
}

rmw_ret_t rmw_get_topic_names_and_types(const rmw_node_t *node, rcutils_allocator_t *a, bool,
                                        rmw_names_and_types_t *out) {
  return all_names_and_types(node, a, out,
                             {rmw_qnx::core::EndpointKind::kPublisher, rmw_qnx::core::EndpointKind::kSubscription});
}
rmw_ret_t rmw_get_service_names_and_types(const rmw_node_t *node, rcutils_allocator_t *a, rmw_names_and_types_t *out) {
  return all_names_and_types(node, a, out,
                             {rmw_qnx::core::EndpointKind::kService, rmw_qnx::core::EndpointKind::kClient});
}

static rmw_ret_t node_names(const rmw_node_t *node, rcutils_string_array_t *names, rcutils_string_array_t *nss,
                            rcutils_string_array_t *enclaves) {
  rmw_ret_t r = check_query_node(node);
  if (r != RMW_RET_OK) return r;
  if (rmw_check_zero_rmw_string_array(names) != RMW_RET_OK) return RMW_RET_INVALID_ARGUMENT;
  if (rmw_check_zero_rmw_string_array(nss) != RMW_RET_OK) return RMW_RET_INVALID_ARGUMENT;
  if (enclaves && rmw_check_zero_rmw_string_array(enclaves) != RMW_RET_OK) return RMW_RET_INVALID_ARGUMENT;
  const auto nodes = load_graph(node->context->impl->domain).nodes();
  rcutils_allocator_t a = rcutils_get_default_allocator();
  if (rcutils_string_array_init(names, nodes.size(), &a) != RCUTILS_RET_OK ||
      rcutils_string_array_init(nss, nodes.size(), &a) != RCUTILS_RET_OK ||
      (enclaves && rcutils_string_array_init(enclaves, nodes.size(), &a) != RCUTILS_RET_OK)) {
    RMW_SET_ERROR_MSG("rmw_qnx: out of memory");
    return RMW_RET_BAD_ALLOC;
  }
  for (size_t i = 0; i < nodes.size(); i++) {
    names->data[i] = rcutils_strdup(nodes[i].name.c_str(), a);
    nss->data[i] = rcutils_strdup(nodes[i].ns.c_str(), a);
    if (enclaves) enclaves->data[i] = rcutils_strdup(nodes[i].enclave.c_str(), a);
  }
  return RMW_RET_OK;
}

rmw_ret_t rmw_get_node_names(const rmw_node_t *node, rcutils_string_array_t *names, rcutils_string_array_t *nss) {
  return node_names(node, names, nss, nullptr);
}
rmw_ret_t rmw_get_node_names_with_enclaves(const rmw_node_t *node, rcutils_string_array_t *names,
                                           rcutils_string_array_t *nss, rcutils_string_array_t *enclaves) {
  RMW_CHECK_ARGUMENT_FOR_NULL(enclaves, RMW_RET_INVALID_ARGUMENT);
  return node_names(node, names, nss, enclaves);
}

static rmw_ret_t count(const rmw_node_t *node, const char *name, size_t *n, rmw_qnx::core::EndpointKind kind) {
  rmw_ret_t r = check_query_node(node);
  if (r != RMW_RET_OK) return r;
  RMW_CHECK_ARGUMENT_FOR_NULL(name, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(n, RMW_RET_INVALID_ARGUMENT);
  int v = 0;
  if (rmw_validate_full_topic_name(name, &v, nullptr) != RMW_RET_OK) return RMW_RET_ERROR;
  if (v != RMW_TOPIC_VALID) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid name: %s", rmw_full_topic_name_validation_result_string(v));
    return RMW_RET_INVALID_ARGUMENT;
  }
  *n = load_graph(node->context->impl->domain).count(kind, name);
  return RMW_RET_OK;
}

rmw_ret_t rmw_count_publishers(const rmw_node_t *node, const char *t, size_t *n) {
  return count(node, t, n, rmw_qnx::core::EndpointKind::kPublisher);
}
rmw_ret_t rmw_count_subscribers(const rmw_node_t *node, const char *t, size_t *n) {
  return count(node, t, n, rmw_qnx::core::EndpointKind::kSubscription);
}
rmw_ret_t rmw_count_clients(const rmw_node_t *node, const char *t, size_t *n) {
  return count(node, t, n, rmw_qnx::core::EndpointKind::kClient);
}
rmw_ret_t rmw_count_services(const rmw_node_t *node, const char *t, size_t *n) {
  return count(node, t, n, rmw_qnx::core::EndpointKind::kService);
}
rmw_ret_t rmw_set_log_severity(rmw_log_severity_t) { return RMW_RET_OK; }

static rmw_ret_t info_by_topic(const rmw_node_t *node, rcutils_allocator_t *a, const char *name,
                               rmw_topic_endpoint_info_array_t *out, rmw_qnx::core::EndpointKind kind) {
  rmw_ret_t r = check_query_node(node);
  if (r != RMW_RET_OK) return r;
  if ((r = check_allocator(a)) != RMW_RET_OK) return r;
  RMW_CHECK_ARGUMENT_FOR_NULL(name, RMW_RET_INVALID_ARGUMENT);
  int v = 0;
  if (rmw_validate_full_topic_name(name, &v, nullptr) != RMW_RET_OK) return RMW_RET_ERROR;
  if (v != RMW_TOPIC_VALID) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid name: %s", rmw_full_topic_name_validation_result_string(v));
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(out, RMW_RET_INVALID_ARGUMENT);
  if (rmw_topic_endpoint_info_array_check_zero(out) != RMW_RET_OK) return RMW_RET_INVALID_ARGUMENT;
  const auto eps = load_graph(node->context->impl->domain).endpoints(kind, name);
  if (eps.empty()) return RMW_RET_OK;
  if ((r = rmw_topic_endpoint_info_array_init_with_size(out, eps.size(), a)) != RMW_RET_OK) return r;
  for (size_t i = 0; i < eps.size(); i++) {
    rmw_topic_endpoint_info_t &info = out->info_array[i];
    info = rmw_get_zero_initialized_topic_endpoint_info();
    rmw_qos_profile_t q = rmw_qos_profile_default;
    q.reliability = rmw_qos_reliability_policy_e(eps[i].qos[0]);
    q.durability = rmw_qos_durability_policy_e(eps[i].qos[1]);
    q.history = rmw_qos_history_policy_e(eps[i].qos[2]);
    q.depth = size_t(eps[i].qos[3]);
    q.liveliness = rmw_qos_liveliness_policy_e(eps[i].qos[4]);
    if (rmw_topic_endpoint_info_set_node_name(&info, eps[i].node_name.c_str(), a) != RMW_RET_OK ||
        rmw_topic_endpoint_info_set_node_namespace(&info, eps[i].node_namespace.c_str(), a) != RMW_RET_OK ||
        rmw_topic_endpoint_info_set_topic_type(&info, eps[i].type.c_str(), a) != RMW_RET_OK ||
        rmw_topic_endpoint_info_set_endpoint_type(
            &info, kind == rmw_qnx::core::EndpointKind::kPublisher ? RMW_ENDPOINT_PUBLISHER : RMW_ENDPOINT_SUBSCRIPTION) !=
            RMW_RET_OK ||
        rmw_topic_endpoint_info_set_gid(&info, eps[i].gid, RMW_GID_STORAGE_SIZE) != RMW_RET_OK ||
        rmw_topic_endpoint_info_set_qos_profile(&info, &q) != RMW_RET_OK) {
      rmw_topic_endpoint_info_array_fini(out, a);
      return RMW_RET_ERROR;
    }
  }
  return RMW_RET_OK;
}

rmw_ret_t rmw_get_publishers_info_by_topic(const rmw_node_t *node, rcutils_allocator_t *a, const char *t, bool,
                                           rmw_topic_endpoint_info_array_t *out) {
  return info_by_topic(node, a, t, out, rmw_qnx::core::EndpointKind::kPublisher);
}
rmw_ret_t rmw_get_subscriptions_info_by_topic(const rmw_node_t *node, rcutils_allocator_t *a, const char *t, bool,
                                              rmw_topic_endpoint_info_array_t *out) {
  return info_by_topic(node, a, t, out, rmw_qnx::core::EndpointKind::kSubscription);
}
rmw_ret_t rmw_publisher_get_network_flow_endpoints(const rmw_publisher_t *, rcutils_allocator_t *,
                                                   rmw_network_flow_endpoint_array_t *) {
  return unsupported("network flow endpoints");
}
rmw_ret_t rmw_subscription_get_network_flow_endpoints(const rmw_subscription_t *, rcutils_allocator_t *,
                                                      rmw_network_flow_endpoint_array_t *) {
  return unsupported("network flow endpoints");
}
rmw_ret_t rmw_subscription_set_on_new_message_callback(rmw_subscription_t *, rmw_event_callback_t, const void *) {
  return unsupported("rmw_subscription_set_on_new_message_callback");
}
rmw_ret_t rmw_event_set_callback(rmw_event_t *ev, rmw_event_callback_t, const void *) {
  CHECK_ID(ev, RMW_RET_INVALID_ARGUMENT);
  return RMW_RET_OK;  // wait sets are the way to learn of an event; the callback is never called
}
bool rmw_feature_supported(rmw_feature_t) { return false; }
rmw_ret_t rmw_take_dynamic_message(const rmw_subscription_t *, rosidl_dynamic_typesupport_dynamic_data_t *, bool *,
                                   rmw_subscription_allocation_t *) {
  return unsupported("dynamic messages");
}
rmw_ret_t rmw_take_dynamic_message_with_info(const rmw_subscription_t *, rosidl_dynamic_typesupport_dynamic_data_t *,
                                             bool *, rmw_message_info_t *, rmw_subscription_allocation_t *) {
  return unsupported("dynamic messages");
}
rmw_ret_t rmw_serialization_support_init(const char *, rcutils_allocator_t *,
                                         rosidl_dynamic_typesupport_serialization_support_t *) {
  return unsupported("dynamic type support");
}

}  // extern "C"
