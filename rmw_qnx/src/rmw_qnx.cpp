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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cdr.hpp"
#include "names.hpp"
#include "qos.hpp"
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
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"
#include "serdes.hpp"

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
  void notify() {
    std::lock_guard<std::mutex> l(mu);
    for (const auto &w : waiters) MsgSendPulse(w.first, w.second, kPulseWake, 0);
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

struct ServiceImpl;

}  // namespace

struct rmw_context_impl_s {
  int chid = -1;
  int self_coid = -1;
  std::string dir;  // service registry of this domain
  std::mutex mu;
  std::map<uint32_t, ServiceImpl *> services;
  uint32_t next_service = 1;
  std::atomic<uint64_t> next_gid{1};
  bool shutdown = false;
};

namespace {

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
  std::deque<std::pair<int64_t, std::vector<uint8_t>>> to_send;
  std::deque<Response> responses;
  Waitable w;
  int chid = -1, coid = -1;
  std::thread sender;
  // Server connection, used by the sender thread only.
  int srv_coid = -1;
  pid_t srv_pid = 0;
  int srv_chid = -1;
  uint32_t srv_index = 0;
};

struct WaitSetImpl {
  rmw_context_impl_s *ctx;
  int chid = -1, coid = -1;  // private channel, used when the wait set holds no service
};

struct NodeImpl {
  GuardCondition graph;  // not triggered yet: graph events come with the /dev/ros2 resource manager
  rmw_guard_condition_t handle;
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

void sender_main(ClientImpl *c) {
  std::vector<uint8_t> reply(sizeof(RepHdr) + kMaxReply);
  for (;;) {
    _pulse pulse;
    if (MsgReceivePulse(c->chid, &pulse, sizeof pulse, nullptr) == -1) continue;
    if (pulse.code == kPulseQuit) return;
    if (pulse.code != kPulseSend) continue;
    // This thread now runs at the priority of the caller that sent the pulse: one request per pulse keeps each
    // request at its own caller's priority.
    std::pair<int64_t, std::vector<uint8_t>> item;
    {
      std::lock_guard<std::mutex> l(c->mu);
      if (c->to_send.empty()) continue;
      item = std::move(c->to_send.front());
      c->to_send.pop_front();
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
    h.seq = item.first;
    std::memcpy(h.guid, c->gid, RMW_GID_STORAGE_SIZE);
    h.len = static_cast<uint32_t>(item.second.size());
    iov_t siov[2], riov[1];
    SETIOV(&siov[0], &h, sizeof h);
    SETIOV(&siov[1], item.second.data(), item.second.size());
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
// priority until it next receives.
void accept_request(rmw_context_impl_s *ctx, rcvid_t rcvid, const _msg_info &info, const uint8_t *buf,
                    size_t got) {
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
  s->w.notify();
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
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(ptr, (ptr)->implementation_identifier, kId,                \
                                   return RMW_RET_INCORRECT_RMW_IMPLEMENTATION)

rmw_ret_t unsupported(const char *what) {
  RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_qnx: %s is not supported yet", what);
  return RMW_RET_UNSUPPORTED;
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
  mkdirs(impl->dir);
  ctx->instance_id = o->instance_id;
  ctx->implementation_identifier = kId;
  ctx->actual_domain_id = domain;
  ctx->options = rmw_get_zero_initialized_init_options();
  rmw_ret_t r = rmw_init_options_copy(o, &ctx->options);
  if (r != RMW_RET_OK) {
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
  return node;
}

rmw_ret_t rmw_destroy_node(rmw_node_t *node) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  rcutils_allocator_t a = rcutils_get_default_allocator();
  a.deallocate(const_cast<char *>(node->name), a.state);
  a.deallocate(const_cast<char *>(node->namespace_), a.state);
  delete static_cast<NodeImpl *>(node->data);
  rmw_node_free(node);
  return RMW_RET_OK;
}

const rmw_guard_condition_t *rmw_node_get_graph_guard_condition(const rmw_node_t *node) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  return &static_cast<NodeImpl *>(node->data)->handle;
}

// ---- guard conditions ----

rmw_guard_condition_t *rmw_create_guard_condition(rmw_context_t *ctx) {
  RMW_CHECK_ARGUMENT_FOR_NULL(ctx, nullptr);
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
      accept_request(ctx, rcvid, info, buf.data(), size_t(got));
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
    for (size_t i = 0; i < subs->subscriber_count; i++) subs->subscribers[i] = nullptr;
  if (evs)
    for (size_t i = 0; i < evs->event_count; i++) evs->events[i] = nullptr;
  return timed_out ? RMW_RET_TIMEOUT : RMW_RET_OK;
}

// ---- services ----

rmw_service_t *rmw_create_service(const rmw_node_t *node, const rosidl_service_type_support_t *ts,
                                  const char *name, const rmw_qos_profile_t *qos) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  if (name[0] == '\0') {
    RMW_SET_ERROR_MSG("empty service name");
    return nullptr;
  }
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
  return srv;
}

rmw_ret_t rmw_destroy_service(rmw_node_t *node, rmw_service_t *srv) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(srv, RMW_RET_INVALID_ARGUMENT);
  auto *s = static_cast<ServiceImpl *>(srv->data);
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
  CdrReader r(p.data.data(), p.data.size());
  if (!r.ok() || !s->req || !rmw_qnx::deserialize_cpp(s->req, ros_request, r)) {
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
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  if (name[0] == '\0') {
    RMW_SET_ERROR_MSG("empty service name");
    return nullptr;
  }
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
  return cli;
}

rmw_ret_t rmw_destroy_client(rmw_node_t *node, rmw_client_t *cli) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(cli, RMW_RET_INVALID_ARGUMENT);
  auto *c = static_cast<ClientImpl *>(cli->data);
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
  {
    std::lock_guard<std::mutex> l(c->mu);
    c->to_send.emplace_back(*seq, std::move(body));
  }
  // Priority -1: the pulse carries this thread's priority to the sender thread.
  if (MsgSendPulse(c->coid, -1, kPulseSend, 0) == -1) {
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
  CdrReader r(resp.data.data(), resp.data.size());
  if (!r.ok() || !c->res || !rmw_qnx::deserialize_cpp(c->res, ros_response, r)) {
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
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(cli, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(available, RMW_RET_INVALID_ARGUMENT);
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
// Topic endpoints, first step: they can be created, report their QoS and gid, and are counted as unmatched, but
// carry no data yet (publish drops, take finds nothing). rclcpp creates some on every node (its time source
// subscribes to /parameter_events), so a service-only application needs them to exist. Delivery over the
// shared-memory ring (core/ring.hpp) is the next step.
struct TopicEndpoint {
  std::string topic;
  rmw_qos_profile_t qos;
  uint8_t gid[RMW_GID_STORAGE_SIZE];
};

rmw_qos_profile_t resolve(const rmw_qos_profile_t &q) {
  rmw_qos_profile_t r = q;
  if (r.history == RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT) r.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  if (r.depth == 0 && r.history == RMW_QOS_POLICY_HISTORY_KEEP_LAST) r.depth = 10;
  if (r.reliability == RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT) r.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
  if (r.durability == RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT) r.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  if (r.liveliness == RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT) r.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
  return r;
}

rmw_publisher_t *rmw_create_publisher(const rmw_node_t *node, const rosidl_message_type_support_t *ts,
                                      const char *name, const rmw_qos_profile_t *qos,
                                      const rmw_publisher_options_t *opts) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(opts, nullptr);
  auto *e = new TopicEndpoint{name, resolve(*qos), {}};
  make_gid(node->context->impl, e->gid);
  rmw_publisher_t *p = rmw_publisher_allocate();
  p->implementation_identifier = kId;
  p->data = e;
  p->topic_name = rcutils_strdup(name, rcutils_get_default_allocator());
  p->options = *opts;
  p->can_loan_messages = false;
  return p;
}
rmw_ret_t rmw_destroy_publisher(rmw_node_t *node, rmw_publisher_t *p) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  delete static_cast<TopicEndpoint *>(p->data);
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
rmw_ret_t rmw_publish(const rmw_publisher_t *p, const void *msg, rmw_publisher_allocation_t *) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(msg, RMW_RET_INVALID_ARGUMENT);
  return RMW_RET_OK;  // no subscriber can match yet
}
rmw_ret_t rmw_publish_loaned_message(const rmw_publisher_t *, void *, rmw_publisher_allocation_t *) {
  return unsupported("loaned messages");
}
rmw_ret_t rmw_publisher_count_matched_subscriptions(const rmw_publisher_t *p, size_t *n) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(n, RMW_RET_INVALID_ARGUMENT);
  *n = 0;
  return RMW_RET_OK;
}
rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t *p, rmw_qos_profile_t *qos) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  *qos = static_cast<TopicEndpoint *>(p->data)->qos;
  return RMW_RET_OK;
}
rmw_ret_t rmw_publisher_event_init(rmw_event_t *, const rmw_publisher_t *, rmw_event_type_t) {
  return unsupported("events");
}
rmw_ret_t rmw_publish_serialized_message(const rmw_publisher_t *, const rmw_serialized_message_t *,
                                         rmw_publisher_allocation_t *) {
  return unsupported("topics");
}
rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t *) { return unsupported("liveliness"); }
rmw_ret_t rmw_publisher_wait_for_all_acked(const rmw_publisher_t *, rmw_time_t) { return unsupported("topics"); }
rmw_ret_t rmw_get_serialized_message_size(const rosidl_message_type_support_t *,
                                          const rosidl_runtime_c__Sequence__bound *, size_t *) {
  return unsupported("rmw_get_serialized_message_size");
}
rmw_ret_t rmw_serialize(const void *, const rosidl_message_type_support_t *, rmw_serialized_message_t *) {
  return unsupported("rmw_serialize");
}
rmw_ret_t rmw_deserialize(const rmw_serialized_message_t *, const rosidl_message_type_support_t *, void *) {
  return unsupported("rmw_deserialize");
}
rmw_ret_t rmw_init_subscription_allocation(const rosidl_message_type_support_t *,
                                           const rosidl_runtime_c__Sequence__bound *, rmw_subscription_allocation_t *) {
  return unsupported("subscription allocation");
}
rmw_ret_t rmw_fini_subscription_allocation(rmw_subscription_allocation_t *) {
  return unsupported("subscription allocation");
}
rmw_subscription_t *rmw_create_subscription(const rmw_node_t *node, const rosidl_message_type_support_t *ts,
                                            const char *name, const rmw_qos_profile_t *qos,
                                            const rmw_subscription_options_t *opts) {
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(node, node->implementation_identifier, kId, return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(ts, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(name, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(opts, nullptr);
  auto *e = new TopicEndpoint{name, resolve(*qos), {}};
  make_gid(node->context->impl, e->gid);
  rmw_subscription_t *s = rmw_subscription_allocate();
  s->implementation_identifier = kId;
  s->data = e;
  s->topic_name = rcutils_strdup(name, rcutils_get_default_allocator());
  s->options = *opts;
  s->can_loan_messages = false;
  s->is_cft_enabled = false;
  return s;
}
rmw_ret_t rmw_destroy_subscription(rmw_node_t *node, rmw_subscription_t *s) {
  CHECK_ID(node, RMW_RET_INVALID_ARGUMENT);
  CHECK_ID(s, RMW_RET_INVALID_ARGUMENT);
  delete static_cast<TopicEndpoint *>(s->data);
  rcutils_allocator_t a = rcutils_get_default_allocator();
  a.deallocate(const_cast<char *>(s->topic_name), a.state);
  rmw_subscription_free(s);
  return RMW_RET_OK;
}
rmw_ret_t rmw_subscription_count_matched_publishers(const rmw_subscription_t *s, size_t *n) {
  CHECK_ID(s, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(n, RMW_RET_INVALID_ARGUMENT);
  *n = 0;
  return RMW_RET_OK;
}
rmw_ret_t rmw_subscription_get_actual_qos(const rmw_subscription_t *s, rmw_qos_profile_t *qos) {
  CHECK_ID(s, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  *qos = static_cast<TopicEndpoint *>(s->data)->qos;
  return RMW_RET_OK;
}
rmw_ret_t rmw_subscription_event_init(rmw_event_t *, const rmw_subscription_t *, rmw_event_type_t) {
  return unsupported("events");
}
rmw_ret_t rmw_subscription_set_content_filter(rmw_subscription_t *, const rmw_subscription_content_filter_options_t *) {
  return unsupported("content filters");
}
rmw_ret_t rmw_subscription_get_content_filter(const rmw_subscription_t *, rcutils_allocator_t *,
                                              rmw_subscription_content_filter_options_t *) {
  return unsupported("content filters");
}
rmw_ret_t rmw_take(const rmw_subscription_t *s, void *, bool *taken, rmw_subscription_allocation_t *) {
  CHECK_ID(s, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  *taken = false;
  return RMW_RET_OK;
}
rmw_ret_t rmw_take_with_info(const rmw_subscription_t *s, void *, bool *taken, rmw_message_info_t *,
                             rmw_subscription_allocation_t *) {
  CHECK_ID(s, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  *taken = false;
  return RMW_RET_OK;
}
rmw_ret_t rmw_take_sequence(const rmw_subscription_t *, size_t, rmw_message_sequence_t *,
                            rmw_message_info_sequence_t *, size_t *, rmw_subscription_allocation_t *) {
  return unsupported("topics");
}
rmw_ret_t rmw_take_serialized_message(const rmw_subscription_t *, rmw_serialized_message_t *, bool *,
                                      rmw_subscription_allocation_t *) {
  return unsupported("topics");
}
rmw_ret_t rmw_take_serialized_message_with_info(const rmw_subscription_t *, rmw_serialized_message_t *, bool *,
                                                rmw_message_info_t *, rmw_subscription_allocation_t *) {
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
rmw_ret_t rmw_take_event(const rmw_event_t *, void *, bool *) { return unsupported("events"); }
bool rmw_event_type_is_supported(rmw_event_type_t) { return false; }
rmw_ret_t rmw_get_publisher_names_and_types_by_node(const rmw_node_t *, rcutils_allocator_t *, const char *,
                                                    const char *, bool, rmw_names_and_types_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_subscriber_names_and_types_by_node(const rmw_node_t *, rcutils_allocator_t *, const char *,
                                                     const char *, bool, rmw_names_and_types_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_service_names_and_types_by_node(const rmw_node_t *, rcutils_allocator_t *, const char *,
                                                  const char *, rmw_names_and_types_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_client_names_and_types_by_node(const rmw_node_t *, rcutils_allocator_t *, const char *,
                                                 const char *, rmw_names_and_types_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_topic_names_and_types(const rmw_node_t *, rcutils_allocator_t *, bool, rmw_names_and_types_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_service_names_and_types(const rmw_node_t *, rcutils_allocator_t *, rmw_names_and_types_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_node_names(const rmw_node_t *, rcutils_string_array_t *, rcutils_string_array_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_node_names_with_enclaves(const rmw_node_t *, rcutils_string_array_t *, rcutils_string_array_t *,
                                           rcutils_string_array_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_count_publishers(const rmw_node_t *, const char *, size_t *) { return unsupported("graph queries"); }
rmw_ret_t rmw_count_subscribers(const rmw_node_t *, const char *, size_t *) { return unsupported("graph queries"); }
rmw_ret_t rmw_count_clients(const rmw_node_t *, const char *, size_t *) { return unsupported("graph queries"); }
rmw_ret_t rmw_count_services(const rmw_node_t *, const char *, size_t *) { return unsupported("graph queries"); }
rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t *p, rmw_gid_t *gid) {
  CHECK_ID(p, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
  gid->implementation_identifier = kId;
  std::memcpy(gid->data, static_cast<TopicEndpoint *>(p->data)->gid, RMW_GID_STORAGE_SIZE);
  return RMW_RET_OK;
}
rmw_ret_t rmw_set_log_severity(rmw_log_severity_t) { return RMW_RET_OK; }
rmw_ret_t rmw_get_publishers_info_by_topic(const rmw_node_t *, rcutils_allocator_t *, const char *, bool,
                                           rmw_topic_endpoint_info_array_t *) {
  return unsupported("graph queries");
}
rmw_ret_t rmw_get_subscriptions_info_by_topic(const rmw_node_t *, rcutils_allocator_t *, const char *, bool,
                                              rmw_topic_endpoint_info_array_t *) {
  return unsupported("graph queries");
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
rmw_ret_t rmw_event_set_callback(rmw_event_t *, rmw_event_callback_t, const void *) {
  return unsupported("events");
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
