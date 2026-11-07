// inversion: the priority-inversion scenario through ROS 2, for any RMW implementation.
//
//   inversion server        priority 10, serves example_interfaces/srv/AddTwoInts with 1 ms of work
//   inversion load          priority 15, busy 20 ms of every 50 ms (no ROS)
//   inversion client [N]    priority 20, one request every 10 ms, 5 ms deadline, then prints the result
//
// The threads the scenario measures are pinned to CPU 0 (the client's calling thread, the server's executor thread,
// the load); the middleware's own threads keep the process's priority and may use either CPU (pin_this_thread). They
// share a log in shared memory (/rmwqnx-inversion, created by the client) with timestamps
// from ClockCycles(), from which the client reports, per request, the priority the server's callback ran at, whether
// the load was mid-burst, who finished first and the deadline misses. The server returns its own current priority in
// the response's `sum` field, so the client also sees it without the log.
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "example_interfaces/srv/add_two_ints.hpp"
#include "rclcpp/rclcpp.hpp"

namespace {

constexpr int kPrioClient = 20, kPrioLoad = 15, kPrioServer = 10;
constexpr int kPeriodUs = 10000, kDeadlineUs = 5000, kWorkUs = 1000, kBurstUs = 20000, kLoadPeriodUs = 50000;
constexpr unsigned kMaxEvents = 65536;
constexpr char kShm[] = "/rmwqnx-inversion";

enum Kind : int32_t { kSend = 1, kSrvBegin, kSrvEnd, kReply, kBurstBegin, kBurstEnd };

struct Event {
  uint64_t t;
  int32_t kind, seq, prio;
};

struct Log {
  std::atomic<unsigned> n;
  std::atomic<int> stop;
  std::atomic<int> server_ready;
  Event ev[kMaxEvents];
};

Log *g_log = nullptr;
uint64_t g_cps = 0;

uint64_t us2cyc(uint64_t us) { return us * g_cps / 1000000u; }

int cur_prio() {
  sched_param p{};
  return SchedGet(0, 0, &p) == -1 ? -1 : p.sched_curpriority;
}

void log_ev(Kind k, int seq) {
  unsigned i = g_log->n.fetch_add(1);
  if (i < kMaxEvents) g_log->ev[i] = Event{ClockCycles(), k, seq, cur_prio()};
}

void pin_this_thread();

void become(int prio) {
  // INVERSION_NO_RT=1: leave priority and CPU alone (to tell middleware faults from scheduling effects).
  if (std::getenv("INVERSION_NO_RT")) return;
  sched_param p{};
  p.sched_priority = prio;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0) {
    std::perror("pthread_setschedparam");
    std::exit(2);
  }
}

// Pins only the calling thread to CPU 0. The middleware's own threads (created by rclcpp::init and the RMW) keep the
// process's priority but may use either CPU: pinned with them, one DDS discovery thread at priority 20 kept CPU 0
// and the priority-10 server was never discovered. The threads pinned here are the ones the scenario measures: the
// client's calling thread, the server's executor thread (which runs the callback) and the load.
void pin_this_thread() {
  if (std::getenv("INVERSION_NO_RT") || std::getenv("INVERSION_NO_PIN")) return;
  if (ThreadCtl(_NTO_TCTL_RUNMASK, reinterpret_cast<void *>(uintptr_t{1})) == -1) {
    std::perror("runmask");
    std::exit(2);
  }
}

Log *map_log(bool create) {
  int fd = shm_open(kShm, create ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR, 0600);
  if (fd < 0) {
    std::perror("shm_open");
    std::exit(3);
  }
  if (create && ftruncate(fd, sizeof(Log)) != 0) {
    std::perror("ftruncate");
    std::exit(3);
  }
  void *p = mmap(nullptr, sizeof(Log), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    std::perror("mmap");
    std::exit(3);
  }
  return static_cast<Log *>(p);
}

volatile uint64_t g_sink;
uint64_t g_work_iters = 0;

void calibrate() {
  const uint64_t n = 1000000, t0 = ClockCycles();
  for (uint64_t i = 0; i < n; i++) g_sink = g_sink + i;
  const uint64_t dt = ClockCycles() - t0;
  g_work_iters = n * us2cyc(kWorkUs) / (dt ? dt : 1);
}

void work() {
  for (uint64_t i = 0; i < g_work_iters; i++) g_sink = g_sink + i;
}

using AddTwoInts = example_interfaces::srv::AddTwoInts;

// The same options on every RMW: only the service under test, without the rosout and parameter topics a node
// creates by default (rmw_qnx has no topics yet).
rclcpp::NodeOptions node_options() {
  return rclcpp::NodeOptions().enable_rosout(false).start_parameter_services(false).start_parameter_event_publisher(
      false);
}

int run_server(int argc, char **argv) {
  become(kPrioServer);
  calibrate();
  g_log = map_log(false);
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("inversion_server", node_options());
  auto srv = node->create_service<AddTwoInts>(
      "inversion", [](const std::shared_ptr<AddTwoInts::Request> req, std::shared_ptr<AddTwoInts::Response> res) {
        const int seq = static_cast<int>(req->a);
        log_ev(kSrvBegin, seq);
        res->sum = cur_prio();
        work();
        log_ev(kSrvEnd, seq);
      });
  g_log->server_ready = 1;
  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(node);
  pin_this_thread();
  while (rclcpp::ok() && !g_log->stop) exec.spin_once(std::chrono::milliseconds(100));
  rclcpp::shutdown();
  return 0;
}

int run_load() {
  become(kPrioLoad);
  pin_this_thread();
  g_log = map_log(false);
  auto next = std::chrono::steady_clock::now();
  int k = 0;
  while (!g_log->stop) {
    log_ev(kBurstBegin, k);
    const uint64_t end = ClockCycles() + us2cyc(kBurstUs);
    while (ClockCycles() < end) g_sink = g_sink + 1;
    log_ev(kBurstEnd, k);
    k++;
    next += std::chrono::microseconds(kLoadPeriodUs);
    std::this_thread::sleep_until(next);
  }
  return 0;
}

bool in_burst(const std::vector<Event> &ev, uint64_t t, uint64_t *burst_end) {
  bool open = false;
  for (const Event &e : ev) {
    if (e.kind == kBurstBegin && e.t <= t) {
      open = true;
    } else if (e.kind == kBurstEnd && open) {
      if (e.t <= t) {
        open = false;
      } else {
        *burst_end = e.t;
        return true;
      }
    }
  }
  return false;
}

int run_client(int argc, char **argv, int nreq, bool verbose) {
  become(kPrioClient);
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("inversion_client", node_options());
  auto cli = node->create_client<AddTwoInts>("inversion");
  if (!cli->wait_for_service(std::chrono::seconds(20))) {
    std::fprintf(stderr, "inversion: service not available\n");
    return 1;
  }
  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(node);
  pin_this_thread();
  // Warm up: the first calls pay for discovery and allocation.
  for (int i = 0; i < 5; i++) {
    auto req = std::make_shared<AddTwoInts::Request>();
    req->a = -1;
    auto f = cli->async_send_request(req);
    exec.spin_until_future_complete(f, std::chrono::seconds(5));
  }
  std::vector<int> prio(nreq, -1);
  std::vector<uint64_t> send(nreq), reply(nreq);
  int lost = 0;
  auto next = std::chrono::steady_clock::now();
  for (int seq = 0; seq < nreq; seq++) {
    auto req = std::make_shared<AddTwoInts::Request>();
    req->a = seq;
    log_ev(kSend, seq);
    send[seq] = ClockCycles();
    auto f = cli->async_send_request(req);
    if (exec.spin_until_future_complete(f, std::chrono::seconds(2)) == rclcpp::FutureReturnCode::SUCCESS) {
      prio[seq] = static_cast<int>(f.get()->sum);
    } else {
      lost++;
    }
    reply[seq] = ClockCycles();
    log_ev(kReply, seq);
    next += std::chrono::microseconds(kPeriodUs);
    std::this_thread::sleep_until(next);
  }
  g_log->stop = 1;

  unsigned n = std::min(g_log->n.load(), kMaxEvents);
  std::vector<Event> ev(g_log->ev, g_log->ev + n);
  std::sort(ev.begin(), ev.end(), [](const Event &a, const Event &b) { return a.t < b.t; });
  std::vector<uint64_t> srv_end(nreq, 0);
  for (const Event &e : ev)
    if (e.kind == kSrvEnd && e.seq >= 0 && e.seq < nreq) srv_end[e.seq] = e.t;
  int misses = 0, contended = 0, server_first = 0, load_first = 0, at_client = 0;
  std::vector<double> rts;
  for (int i = 0; i < nreq; i++) {
    const double rt = double(reply[i] - send[i]) * 1e6 / double(g_cps);
    rts.push_back(rt);
    const bool miss = rt > kDeadlineUs;
    misses += miss;
    at_client += prio[i] == kPrioClient;
    uint64_t bend = 0;
    const bool c = in_burst(ev, send[i], &bend);
    const char *who = "-";
    if (c) {
      contended++;
      if (srv_end[i] && srv_end[i] < bend) {
        server_first++;
        who = "server";
      } else {
        load_first++;
        who = "load";
      }
    }
    if (verbose)
      std::printf("req %4d prio %2d response_us %8.1f contended %d ran_first %-6s %s\n", i, prio[i], rt, c, who,
                  miss ? "MISS" : "ok");
  }
  std::sort(rts.begin(), rts.end());
  const char *rmw = std::getenv("RMW_IMPLEMENTATION");
  std::printf(
      "RESULT rmw=%s requests=%d deadline_us=%d misses=%d lost=%d contended=%d server_ran_first=%d "
      "load_ran_first=%d served_at_client_prio=%d p50_us=%.1f max_us=%.1f\n",
      rmw ? rmw : "default", nreq, kDeadlineUs, misses, lost, contended, server_first, load_first, at_client,
      rts[rts.size() / 2], rts.back());
  rclcpp::shutdown();
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
  if (argc < 2) {
    std::fprintf(stderr, "usage: inversion server|load|client [REQUESTS] [-v]\n");
    return 64;
  }
  const std::string role = argv[1];
  if (role == "init") {
    // Creates the shared log; run once before the other three.
    map_log(true);
    return 0;
  }
  if (role == "client") {
    int nreq = argc > 2 ? std::atoi(argv[2]) : 200;
    if (nreq < 1 || nreq > 5000) nreq = 200;
    const bool verbose = argc > 3 && std::strcmp(argv[3], "-v") == 0;
    g_log = map_log(false);
    return run_client(argc, argv, nreq, verbose);
  }
  if (role == "server") return run_server(argc, argv);
  if (role == "load") return run_load();
  std::fprintf(stderr, "inversion: unknown role %s\n", role.c_str());
  return 64;
}
