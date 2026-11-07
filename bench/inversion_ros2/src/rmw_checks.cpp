// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// Behaviour checks of rmw_qnx that only the guest can run: priorities, kernel objects, process limits. Each check
// runs in its own child process, so a crash fails that check and the others still run. Called through
// rmw_implementation, so run it with RMW_IMPLEMENTATION=rmw_qnx.
//   rmw_checks [name...]          (default: every check); prints PASS or FAIL per check, exit status = failures
#include <sched.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "example_interfaces/msg/int32.hpp"
#include "example_interfaces/msg/string.hpp"
#include "example_interfaces/srv/add_two_ints.hpp"
#include "rcl_interfaces/srv/get_parameters.hpp"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rosidl_typesupport_cpp/message_type_support.hpp"
#include "rosidl_typesupport_cpp/service_type_support.hpp"

namespace {

#define EXPECT(cond, ...)                                     \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("  expected %s: ", #cond);                  \
      std::printf(__VA_ARGS__);                               \
      std::printf(" (%s)\n", rmw_get_error_string().str);     \
      return false;                                           \
    }                                                         \
  } while (0)

struct Ros {
  rmw_init_options_t opts = rmw_get_zero_initialized_init_options();
  rmw_context_t ctx = rmw_get_zero_initialized_context();
  rmw_node_t *node = nullptr;
  bool up(const char *name) {
    if (rmw_init_options_init(&opts, rcutils_get_default_allocator()) != RMW_RET_OK) return false;
    opts.enclave = rcutils_strdup("/", rcutils_get_default_allocator());
    if (rmw_init(&opts, &ctx) != RMW_RET_OK) return false;
    node = rmw_create_node(&ctx, name, "/");
    return node != nullptr;
  }
};

int cur_priority() {
  sched_param p{};
  SchedGet(0, 0, &p);
  return p.sched_curpriority;
}

void set_priority(int prio) {
  sched_param p{};
  p.sched_priority = prio;
  pthread_setschedparam(pthread_self(), SCHED_RR, &p);
}

rmw_qos_profile_t qos() { return rmw_qos_profile_default; }

// Two threads at different priorities call rmw_send_request on one client while its sender thread is busy with an
// earlier request. Each request must reach the server at its own caller's priority.
bool two_callers_on_one_client_are_each_served_at_their_own_priority() {
  using Srv = example_interfaces::srv::AddTwoInts;
  const auto *ts = rosidl_typesupport_cpp::get_service_type_support_handle<Srv>();
  Ros server, client;
  EXPECT(server.up("prio_server") && client.up("prio_client"), "init");
  const std::string name = "/rmwqnx_check_prio_" + std::to_string(getpid());
  rmw_qos_profile_t q = rmw_qos_profile_services_default;
  rmw_service_t *srv = rmw_create_service(server.node, ts, name.c_str(), &q);
  rmw_client_t *cli = rmw_create_client(client.node, ts, name.c_str(), &q);
  EXPECT(srv && cli, "service and client");
  rmw_wait_set_t *ws = rmw_create_wait_set(&server.ctx, 1);
  EXPECT(ws, "wait set");

  std::atomic<bool> first_taken{false}, both_sent{false};
  std::vector<std::pair<int64_t, int>> served;  // (request.a = caller's priority, server thread's priority)
  std::thread st([&] {
    set_priority(5);
    for (int k = 0; k < 3; k++) {
      Srv::Request req;
      rmw_service_info_t info{};
      bool taken = false;
      for (int tries = 0; tries < 50 && !taken; tries++) {
        void *one[1] = {srv->data};
        rmw_services_t srvs{1, one};
        rmw_time_t t{1, 0};
        rmw_wait(nullptr, nullptr, &srvs, nullptr, nullptr, ws, &t);
        rmw_take_request(srv, &info, &req, &taken);
      }
      if (!taken) return;
      if (k == 0) {
        first_taken = true;
        while (!both_sent) usleep(1000);  // the client's sender thread stays blocked in MsgSend meanwhile
      } else {
        served.emplace_back(req.a, cur_priority());
      }
      Srv::Response res;
      rmw_send_response(srv, &info.request_id, &res);
    }
  });
  set_priority(15);
  Srv::Request r0;
  r0.a = 15;
  int64_t seq = 0;
  EXPECT(rmw_send_request(cli, &r0, &seq) == RMW_RET_OK, "first request");
  for (int i = 0; i < 5000 && !first_taken; i++) usleep(1000);
  EXPECT(first_taken.load(), "server took the first request");
  auto caller = [&](int prio) {
    return std::thread([&, prio] {
      set_priority(prio);
      Srv::Request r;
      r.a = prio;
      int64_t s = 0;
      rmw_send_request(cli, &r, &s);
    });
  };
  std::thread low = caller(10);
  low.join();
  std::thread high = caller(20);
  high.join();
  both_sent = true;
  st.join();
  EXPECT(served.size() == 2, "two requests served, got %zu", served.size());
  for (const auto &s : served) std::printf("  request from priority %lld served at priority %d\n", (long long)s.first, s.second);
  for (const auto &s : served) EXPECT(s.first == s.second, "served at the caller's priority");
  return true;
}

// The topic table holds 32 subscribers. The 33rd must fail rather than be created and never woken.
bool the_33rd_subscription_on_a_topic_fails_and_leaves_32_registered() {
  const auto *ts = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::String>();
  Ros r;
  EXPECT(r.up("many_subs"), "init");
  const std::string name = "/rmwqnx_check_subs_" + std::to_string(getpid());
  rmw_qos_profile_t q = qos();
  rmw_subscription_options_t so = rmw_get_default_subscription_options();
  rmw_publisher_options_t po = rmw_get_default_publisher_options();
  std::vector<rmw_subscription_t *> subs;
  for (int i = 0; i < 32; i++) {
    subs.push_back(rmw_create_subscription(r.node, ts, name.c_str(), &q, &so));
    EXPECT(subs.back(), "subscription %d created", i + 1);
  }
  rmw_subscription_t *extra = rmw_create_subscription(r.node, ts, name.c_str(), &q, &so);
  EXPECT(extra == nullptr, "the 33rd subscription to fail");
  rmw_reset_error();
  rmw_publisher_t *pub = rmw_create_publisher(r.node, ts, name.c_str(), &q, &po);
  size_t n = 0;
  EXPECT(pub && rmw_publisher_count_matched_subscriptions(pub, &n) == RMW_RET_OK && n == 32, "32 matched, got %zu", n);
  EXPECT(rmw_destroy_subscription(r.node, subs.back()) == RMW_RET_OK, "destroy one");
  subs.back() = rmw_create_subscription(r.node, ts, name.c_str(), &q, &so);
  EXPECT(subs.back(), "a subscription fits again after one left");
  return true;
}

rmw_serialized_message_t message_over(uint8_t *bytes, size_t len) {
  rmw_serialized_message_t m = rmw_get_zero_initialized_serialized_message();
  m.buffer = bytes;
  m.buffer_length = len;
  m.buffer_capacity = len;
  m.allocator = rcutils_get_default_allocator();
  return m;
}

// GetParameters_Response starts with a sequence of ParameterValue messages; a length of 2^32-1 in an 8-byte
// buffer must be refused before anything is allocated for it.
bool a_forged_nested_sequence_length_is_rejected() {
  using Res = rcl_interfaces::srv::GetParameters_Response;
  const auto *ts = rosidl_typesupport_cpp::get_message_type_support_handle<Res>();
  uint8_t bytes[] = {0x00, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
  rmw_serialized_message_t m = message_over(bytes, sizeof bytes);
  Res out;
  const rmw_ret_t ret = rmw_deserialize(&m, ts, &out);
  EXPECT(ret == RMW_RET_ERROR, "RMW_RET_ERROR, got %d", int(ret));
  EXPECT(out.values.empty(), "nothing allocated, got %zu values", out.values.size());
  return true;
}

bool a_null_buffer_with_a_length_is_rejected() {
  const auto *ts = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::String>();
  rmw_serialized_message_t m = message_over(nullptr, 8);
  example_interfaces::msg::String out;
  const rmw_ret_t ret = rmw_deserialize(&m, ts, &out);
  EXPECT(ret == RMW_RET_INVALID_ARGUMENT, "RMW_RET_INVALID_ARGUMENT, got %d", int(ret));
  return true;
}

bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

// A topic whose last endpoint is destroyed is gone: from /dev/shmem, from /dev/ros2 (when ros2graphd runs), and a
// publisher of another type may then use the name.
bool a_destroyed_topic_is_gone_and_its_name_takes_another_type() {
  const auto *str = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::String>();
  const auto *i32 = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::Int32>();
  Ros r;
  EXPECT(r.up("retype"), "init");
  const std::string leaf = "rmwqnx_check_retype_" + std::to_string(getpid());
  const std::string name = "/" + leaf;
  const std::string shm = "/dev/shmem/rmwqnx.d0.%2F" + leaf;
  const bool graphd = exists("/dev/ros2/topics");
  rmw_qos_profile_t q = qos();
  rmw_publisher_options_t po = rmw_get_default_publisher_options();
  rmw_subscription_options_t so = rmw_get_default_subscription_options();
  rmw_publisher_t *p = rmw_create_publisher(r.node, str, name.c_str(), &q, &po);
  rmw_subscription_t *s = rmw_create_subscription(r.node, str, name.c_str(), &q, &so);
  EXPECT(p && s, "String endpoints");
  EXPECT(exists(shm), "%s while endpoints exist", shm.c_str());
  if (graphd) EXPECT(exists("/dev/ros2/topics/" + leaf), "listed in /dev/ros2/topics");
  EXPECT(rmw_destroy_publisher(r.node, p) == RMW_RET_OK, "destroy publisher");
  EXPECT(exists(shm), "still there while the subscription lives");
  EXPECT(rmw_destroy_subscription(r.node, s) == RMW_RET_OK, "destroy subscription");
  EXPECT(!exists(shm), "%s gone after the last endpoint", shm.c_str());
  if (graphd) EXPECT(!exists("/dev/ros2/topics/" + leaf), "gone from /dev/ros2/topics");
  rmw_publisher_t *p2 = rmw_create_publisher(r.node, i32, name.c_str(), &q, &po);
  EXPECT(p2, "an Int32 publisher on the same name");
  std::printf("  /dev/ros2 checked: %s\n", graphd ? "yes" : "no (ros2graphd not running)");
  return true;
}

// Endpoints joining and leaving one topic from two threads: every publication reaches a subscription created on
// the same name just before it, so no endpoint is ever left on a topic object being removed.
bool endpoints_coming_and_going_on_one_topic_never_split_it() {
  const auto *str = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::String>();
  Ros r;
  EXPECT(r.up("churn"), "init");
  const std::string name = "/rmwqnx_check_churn_" + std::to_string(getpid());
  std::atomic<int> lost{0}, failed{0};
  auto worker = [&] {
    rmw_qos_profile_t q = qos();
    rmw_publisher_options_t po = rmw_get_default_publisher_options();
    rmw_subscription_options_t so = rmw_get_default_subscription_options();
    for (int i = 0; i < 200; i++) {
      rmw_subscription_t *s = rmw_create_subscription(r.node, str, name.c_str(), &q, &so);
      rmw_publisher_t *p = rmw_create_publisher(r.node, str, name.c_str(), &q, &po);
      if (!s || !p) {
        failed++;
        if (s) rmw_destroy_subscription(r.node, s);
        if (p) rmw_destroy_publisher(r.node, p);
        continue;
      }
      example_interfaces::msg::String m;
      m.data = "x";
      rmw_publish(p, &m, nullptr);
      bool taken = false;
      example_interfaces::msg::String got;
      for (int k = 0; k < 4 && !taken; k++) rmw_take(s, &got, &taken, nullptr);
      if (!taken) lost++;
      rmw_destroy_publisher(r.node, p);
      rmw_destroy_subscription(r.node, s);
    }
  };
  std::thread a(worker), b(worker);
  a.join();
  b.join();
  std::printf("  400 rounds: %d endpoint creations failed, %d publications not received\n", failed.load(), lost.load());
  EXPECT(failed == 0 && lost == 0, "no failure");
  EXPECT(!exists("/dev/shmem/rmwqnx.d0.%2F" + name.substr(1)), "the topic is gone at the end");
  return true;
}

// Another process creates a node and a publisher: this process's graph guard condition fires, and the queries and
// /dev/ros2/nodes (when ros2graphd runs) see them; when that process ends, they are gone.
bool graph_queries_see_another_process_and_its_end() {
  const auto *str = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::String>();
  const std::string leaf = "rmwqnx_graph_" + std::to_string(getpid());
  const std::string topic = "/" + leaf;
  int ready[2], done[2];
  if (pipe(ready) != 0 || pipe(done) != 0) return false;
  const pid_t child = fork();
  if (child == 0) {
    Ros r;
    if (!r.up(("remote_" + leaf).c_str())) _exit(2);
    rmw_qos_profile_t q = qos();
    rmw_publisher_options_t po = rmw_get_default_publisher_options();
    if (!rmw_create_publisher(r.node, str, topic.c_str(), &q, &po)) _exit(3);
    char c = 1;
    if (write(ready[1], &c, 1) != 1) _exit(4);
    if (read(done[0], &c, 1) != 1) _exit(5);
    _exit(0);  // without destroying anything: its process ending must be enough
  }
  Ros r;
  EXPECT(r.up("local"), "init");
  char c;
  EXPECT(read(ready[0], &c, 1) == 1, "the other process is ready");
  size_t n = 0;
  EXPECT(rmw_count_publishers(r.node, topic.c_str(), &n) == RMW_RET_OK && n == 1, "1 publisher, got %zu", n);
  rcutils_string_array_t names = rcutils_get_zero_initialized_string_array();
  rcutils_string_array_t nss = rcutils_get_zero_initialized_string_array();
  EXPECT(rmw_get_node_names(r.node, &names, &nss) == RMW_RET_OK, "node names");
  bool found = false;
  for (size_t i = 0; i < names.size; i++) found |= std::string(names.data[i]) == "remote_" + leaf;
  std::printf("  %zu nodes listed, the other process's among them: %s\n", names.size, found ? "yes" : "no");
  EXPECT(found, "the other node listed");
  rmw_names_and_types_t nt = rmw_get_zero_initialized_names_and_types();
  rcutils_allocator_t a = rcutils_get_default_allocator();
  EXPECT(rmw_get_topic_names_and_types(r.node, &a, false, &nt) == RMW_RET_OK, "topic names");
  bool typed = false;
  for (size_t i = 0; i < nt.names.size; i++)
    typed |= std::string(nt.names.data[i]) == topic && nt.types[i].size == 1 &&
             std::string(nt.types[i].data[0]) == "example_interfaces/msg/String";
  EXPECT(typed, "the topic with its type");
  if (exists("/dev/ros2/nodes")) EXPECT(exists("/dev/ros2/nodes/remote_" + leaf), "listed in /dev/ros2/nodes");
  if (write(done[1], &c, 1) != 1) return false;
  int status = 0;
  waitpid(child, &status, 0);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the other process ended cleanly");
  n = 1;
  EXPECT(rmw_count_publishers(r.node, topic.c_str(), &n) == RMW_RET_OK && n == 0, "0 publishers after it ended, got %zu", n);
  if (exists("/dev/ros2/nodes")) EXPECT(!exists("/dev/ros2/nodes/remote_" + leaf), "gone from /dev/ros2/nodes");
  return true;
}

// Waits up to `ms` on one event; true if the wait set reports it ready.
bool wait_event(rmw_wait_set_t *ws, rmw_event_t *ev, int ms) {
  void *one[1] = {ev};
  rmw_events_t evs{1, one};
  rmw_time_t t{time_t(ms / 1000), long(ms % 1000) * 1000000};
  (void)rmw_wait(nullptr, nullptr, nullptr, nullptr, &evs, ws, &t);
  return one[0] != nullptr;
}

int64_t ms_since(const timespec &t0) {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
}

// A publisher with a 100 ms deadline publishes once and goes quiet for 450 ms: the event wakes a waiting thread by
// a timer pulse (no polling thread) and the total reaches exactly 4, one per elapsed period. A healthy publisher
// (200 ms deadline, a message every 20 ms for 1.5 s) raises none.
bool a_quiet_publisher_raises_exactly_its_deadline_events_and_a_healthy_one_none() {
  const auto *str = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::String>();
  Ros r;
  EXPECT(r.up("deadline"), "init");
  rmw_publisher_options_t po = rmw_get_default_publisher_options();
  rmw_wait_set_t *ws = rmw_create_wait_set(&r.ctx, 1);
  EXPECT(ws, "wait set");
  EXPECT(rmw_event_type_is_supported(RMW_EVENT_OFFERED_DEADLINE_MISSED), "deadline events supported");

  rmw_qos_profile_t q = qos();
  q.deadline = rmw_time_t{0, 200 * 1000000};
  const std::string healthy_name = "/rmwqnx_check_healthy_" + std::to_string(getpid());
  rmw_publisher_t *healthy = rmw_create_publisher(r.node, str, healthy_name.c_str(), &q, &po);
  EXPECT(healthy, "healthy publisher");
  rmw_event_t hev = rmw_get_zero_initialized_event();
  EXPECT(rmw_publisher_event_init(&hev, healthy, RMW_EVENT_OFFERED_DEADLINE_MISSED) == RMW_RET_OK, "event init");
  example_interfaces::msg::String m;
  m.data = "x";
  timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int taken_count = 0;
  while (ms_since(t0) < 1500) {
    EXPECT(rmw_publish(healthy, &m, nullptr) == RMW_RET_OK, "publish");
    usleep(20000);
    rmw_offered_deadline_missed_status_t st{};
    bool taken = false;
    EXPECT(rmw_take_event(&hev, &st, &taken) == RMW_RET_OK, "take");
    taken_count += taken;
  }
  std::printf("  healthy publisher, deadline 200 ms, a message every 20 ms for 1.5 s: %d events\n", taken_count);
  EXPECT(taken_count == 0, "no event from a healthy publisher");

  q.deadline = rmw_time_t{0, 100 * 1000000};
  const std::string quiet_name = "/rmwqnx_check_quiet_" + std::to_string(getpid());
  rmw_publisher_t *quiet = rmw_create_publisher(r.node, str, quiet_name.c_str(), &q, &po);
  EXPECT(quiet, "quiet publisher");
  rmw_event_t qev = rmw_get_zero_initialized_event();
  EXPECT(rmw_publisher_event_init(&qev, quiet, RMW_EVENT_OFFERED_DEADLINE_MISSED) == RMW_RET_OK, "event init");
  EXPECT(rmw_publish(quiet, &m, nullptr) == RMW_RET_OK, "publish once");
  clock_gettime(CLOCK_MONOTONIC, &t0);
  EXPECT(wait_event(ws, &qev, 2000), "the wait set woke for the event");
  const int64_t woke_ms = ms_since(t0);
  rmw_offered_deadline_missed_status_t st{};
  bool taken = false;
  EXPECT(rmw_take_event(&qev, &st, &taken) == RMW_RET_OK && taken, "first event taken");
  std::printf("  quiet publisher: woke %lld ms after its last message; first status total %d, change %d\n",
              (long long)woke_ms, st.total_count, st.total_count_change);
  EXPECT(st.total_count == st.total_count_change && st.total_count >= 1, "first status counts only the new events");
  while (ms_since(t0) < 450) usleep(5000);
  EXPECT(rmw_take_event(&qev, &st, &taken) == RMW_RET_OK, "take");
  const int total = taken ? st.total_count : 1;
  std::printf("  after 450 ms of silence: total %d\n", total);
  EXPECT(total == 4, "4 elapsed periods in 450 ms, got %d", total);
  return true;
}

// A publisher's matched event reports a subscription joining and leaving, woken through the graph pulse.
bool a_matched_event_reports_a_subscription_joining_and_leaving() {
  const auto *str = rosidl_typesupport_cpp::get_message_type_support_handle<example_interfaces::msg::String>();
  Ros r;
  EXPECT(r.up("matched"), "init");
  const std::string name = "/rmwqnx_check_matched_" + std::to_string(getpid());
  rmw_qos_profile_t q = qos();
  rmw_publisher_options_t po = rmw_get_default_publisher_options();
  rmw_subscription_options_t so = rmw_get_default_subscription_options();
  rmw_publisher_t *p = rmw_create_publisher(r.node, str, name.c_str(), &q, &po);
  EXPECT(p, "publisher");
  rmw_event_t ev = rmw_get_zero_initialized_event();
  EXPECT(rmw_publisher_event_init(&ev, p, RMW_EVENT_PUBLICATION_MATCHED) == RMW_RET_OK, "event init");
  rmw_wait_set_t *ws = rmw_create_wait_set(&r.ctx, 1);
  EXPECT(!wait_event(ws, &ev, 100), "nothing yet");
  rmw_subscription_t *s = rmw_create_subscription(r.node, str, name.c_str(), &q, &so);
  EXPECT(s, "subscription");
  EXPECT(wait_event(ws, &ev, 2000), "woken by the join");
  rmw_matched_status_t st{};
  bool taken = false;
  EXPECT(rmw_take_event(&ev, &st, &taken) == RMW_RET_OK && taken, "join taken");
  std::printf("  join: total %zu (%+d), current %zu (%+d)\n", st.total_count, st.total_count_change, st.current_count,
              st.current_count_change);
  EXPECT(st.total_count == 1 && st.total_count_change == 1 && st.current_count == 1 && st.current_count_change == 1,
         "one join");
  EXPECT(rmw_destroy_subscription(r.node, s) == RMW_RET_OK, "destroy");
  EXPECT(wait_event(ws, &ev, 2000), "woken by the leave");
  EXPECT(rmw_take_event(&ev, &st, &taken) == RMW_RET_OK && taken, "leave taken");
  std::printf("  leave: total %zu (%+d), current %zu (%+d)\n", st.total_count, st.total_count_change, st.current_count,
              st.current_count_change);
  EXPECT(st.total_count == 1 && st.total_count_change == 0 && st.current_count == 0 && st.current_count_change == -1,
         "one leave");
  return true;
}

// Observation, run only when named: the priority a thread has after it receives a pulse at 20 and then one at 10,
// and after it receives a message from a sender at 20 and then one from a sender at 10.
bool observe_priority_after_receive() {
  set_priority(5);
  const int chid = ChannelCreate(0);
  const int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
  _pulse p;
  MsgSendPulse(coid, 20, _PULSE_CODE_MINAVAIL, 0);
  MsgSendPulse(coid, 10, _PULSE_CODE_MINAVAIL, 0);
  MsgReceivePulse(chid, &p, sizeof p, nullptr);
  std::printf("  base 5, pulse at 20 received: priority %d\n", cur_priority());
  MsgReceivePulse(chid, &p, sizeof p, nullptr);
  std::printf("  then pulse at 10 received: priority %d\n", cur_priority());
  set_priority(5);
  for (int prio : {20, 10}) {
    std::thread t([&, prio] {
      set_priority(prio);
      int v = 0;
      MsgSend(coid, &v, sizeof v, &v, sizeof v);
    });
    usleep(20000);
    int v = 0;
    const rcvid_t r = MsgReceive(chid, &v, sizeof v, nullptr);
    std::printf("  message from priority %d received: priority %d\n", prio, cur_priority());
    MsgReply(r, 0, &v, sizeof v);
    t.join();
  }
  return true;
}

const std::vector<std::pair<const char *, std::function<bool()>>> kObservations = {
    {"observe_priority_after_receive", observe_priority_after_receive},
};

const std::vector<std::pair<const char *, std::function<bool()>>> kChecks = {
    {"two_callers_on_one_client_are_each_served_at_their_own_priority",
     two_callers_on_one_client_are_each_served_at_their_own_priority},
    {"the_33rd_subscription_on_a_topic_fails_and_leaves_32_registered",
     the_33rd_subscription_on_a_topic_fails_and_leaves_32_registered},
    {"a_forged_nested_sequence_length_is_rejected", a_forged_nested_sequence_length_is_rejected},
    {"a_null_buffer_with_a_length_is_rejected", a_null_buffer_with_a_length_is_rejected},
    {"a_destroyed_topic_is_gone_and_its_name_takes_another_type",
     a_destroyed_topic_is_gone_and_its_name_takes_another_type},
    {"endpoints_coming_and_going_on_one_topic_never_split_it", endpoints_coming_and_going_on_one_topic_never_split_it},
    {"graph_queries_see_another_process_and_its_end", graph_queries_see_another_process_and_its_end},
    {"a_quiet_publisher_raises_exactly_its_deadline_events_and_a_healthy_one_none",
     a_quiet_publisher_raises_exactly_its_deadline_events_and_a_healthy_one_none},
    {"a_matched_event_reports_a_subscription_joining_and_leaving",
     a_matched_event_reports_a_subscription_joining_and_leaving},
};

}  // namespace

int main(int argc, char **argv) {
  int failures = 0, run = 0;
  std::vector<std::pair<const char *, std::function<bool()>>> all = kChecks;
  if (argc >= 2) all.insert(all.end(), kObservations.begin(), kObservations.end());
  for (const auto &c : all) {
    bool wanted = argc < 2;
    for (int i = 1; i < argc; i++) wanted |= std::strcmp(argv[i], c.first) == 0;
    if (!wanted) continue;
    run++;
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
      alarm(60);
      const bool ok = c.second();
      std::fflush(stdout);
      _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (WIFSIGNALED(status)) std::printf("  child ended by signal %d\n", WTERMSIG(status));
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.first);
    failures += !ok;
  }
  std::printf("checks: %d passed, %d failed\n", run - failures, failures);
  return failures;
}
