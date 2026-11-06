/*
 * pinv: does a request carry its sender's priority to the code that serves it?
 *
 * Three processes share CPU 0:
 *   client  priority 20  sends one request every PERIOD and expects the reply within DEADLINE
 *   load    priority 15  busy for BURST out of every LOAD_PERIOD (a medium-priority hog)
 *   server  priority 10  serves each request with WORK of computation
 *
 * The request travels over one of three transports:
 *   msg   QNX MsgSend/MsgReply on a named channel (priority inheritance is the channel default)
 *   udp   a UDP socket on the loopback interface, through io-sock
 *   shm   a shared-memory slot with two process-shared semaphores (no inheritance)
 *
 * With msg the server is raised to 20 when it receives, so it runs ahead of the load. With udp and shm it stays at
 * 10 and waits for the load to finish its burst: classic priority inversion. Every process writes timestamped events
 * to a shared log; the parent reads it afterwards and prints, per request, the server's priority while it worked,
 * whether the load was mid-burst, who ran first, and the deadline misses.
 *
 *   pinv MODE [-n REQUESTS] [-l 0|1] [-v] [-m MARKER_DEVICE]
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/socket.h>
#include <sys/syspage.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PRIO_CLIENT 20
#define PRIO_LOAD 15
#define PRIO_SERVER 10

#define PERIOD_US 10000      /* client request period */
#define DEADLINE_US 5000     /* reply must arrive within this */
#define WORK_US 1000         /* server computation per request */
#define BURST_US 20000       /* load busy time per load period */
#define LOAD_PERIOD_US 50000 /* load period */
#define UDP_PORT 47110
#define MAX_EVENTS 65536

enum ev_kind { EV_SEND = 1, EV_SRV_BEGIN, EV_SRV_END, EV_REPLY, EV_BURST_BEGIN, EV_BURST_END };

struct event {
    uint64_t t;
    int32_t kind;
    int32_t seq;
    int32_t prio; /* current priority of the thread that logged it */
};

struct shared {
    atomic_uint n;
    volatile int stop;
    volatile int server_ready;
    sem_t req, rep;
    int32_t slot;
    struct event ev[MAX_EVENTS];
};

static struct shared *S;
static uint64_t cps; /* ClockCycles per second */

static uint64_t us2cyc(uint64_t us) { return us * cps / 1000000u; }

static int cur_prio(void) {
    struct sched_param p;
    if (SchedGet(0, 0, &p) == -1) return -1;
    return p.sched_curpriority;
}

static void log_ev(int kind, int seq) {
    unsigned i = atomic_fetch_add(&S->n, 1);
    if (i >= MAX_EVENTS) return;
    S->ev[i].t = ClockCycles();
    S->ev[i].kind = kind;
    S->ev[i].seq = seq;
    S->ev[i].prio = cur_prio();
}

static void become(int prio) {
    struct sched_param p = {.sched_priority = prio};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0) {
        perror("pthread_setschedparam");
        exit(2);
    }
    /* Everything that competes runs on CPU 0, so priority alone decides who runs. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        perror("ThreadCtl runmask");
        exit(2);
    }
}

/* Work measured in loop iterations, not wall time, so a preempted server still owes the full amount. */
static volatile uint64_t sink;
static uint64_t work_iters;

static void calibrate(void) {
    uint64_t n = 1000000, t0 = ClockCycles();
    for (uint64_t i = 0; i < n; i++) sink += i;
    uint64_t dt = ClockCycles() - t0;
    work_iters = n * us2cyc(WORK_US) / (dt ? dt : 1);
}

static void work(void) {
    for (uint64_t i = 0; i < work_iters; i++) sink += i;
}

static void spin_until(uint64_t t_end) {
    while (ClockCycles() < t_end) sink++;
}

static void sleep_until_ns(struct timespec *ts, long add_ns) {
    ts->tv_nsec += add_ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ts, NULL) == EINTR) {}
}

/* ---- load ---- */
static void run_load(void) {
    become(PRIO_LOAD);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int k = 0;
    while (!S->stop) {
        log_ev(EV_BURST_BEGIN, k);
        spin_until(ClockCycles() + us2cyc(BURST_US));
        log_ev(EV_BURST_END, k);
        k++;
        sleep_until_ns(&ts, LOAD_PERIOD_US * 1000L);
    }
    exit(0);
}

/* ---- servers ---- */
#define NAME "pinv-server"

static void serve_msg(void) {
    name_attach_t *att = name_attach(NULL, NAME, 0);
    if (!att) {
        perror("name_attach");
        exit(3);
    }
    S->server_ready = 1;
    for (;;) {
        int32_t seq;
        rcvid_t rcvid = MsgReceive(att->chid, &seq, sizeof seq, NULL);
        if (rcvid == -1) continue;
        if (rcvid == 0) continue; /* pulse */
        if (seq < 0) {
            MsgReply(rcvid, 0, NULL, 0);
            exit(0);
        }
        log_ev(EV_SRV_BEGIN, seq);
        work();
        log_ev(EV_SRV_END, seq);
        MsgReply(rcvid, 0, &seq, sizeof seq);
    }
}

static void serve_udp(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(UDP_PORT)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("udp bind");
        exit(3);
    }
    S->server_ready = 1;
    for (;;) {
        int32_t seq;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        if (recvfrom(s, &seq, sizeof seq, 0, (struct sockaddr *)&from, &fl) != sizeof seq) continue;
        if (seq < 0) {
            sendto(s, &seq, sizeof seq, 0, (struct sockaddr *)&from, fl);
            exit(0);
        }
        log_ev(EV_SRV_BEGIN, seq);
        work();
        log_ev(EV_SRV_END, seq);
        sendto(s, &seq, sizeof seq, 0, (struct sockaddr *)&from, fl);
    }
}

static void serve_shm(void) {
    S->server_ready = 1;
    for (;;) {
        while (sem_wait(&S->req) == -1 && errno == EINTR) {}
        int32_t seq = S->slot;
        if (seq < 0) {
            sem_post(&S->rep);
            exit(0);
        }
        log_ev(EV_SRV_BEGIN, seq);
        work();
        log_ev(EV_SRV_END, seq);
        sem_post(&S->rep);
    }
}

/* ---- client transports ---- */
static int coid = -1, usock = -1;
static struct sockaddr_in udp_srv;

static void client_open(const char *mode) {
    if (!strcmp(mode, "msg")) {
        coid = name_open(NAME, 0);
        if (coid == -1) {
            perror("name_open");
            exit(4);
        }
    } else if (!strcmp(mode, "udp")) {
        usock = socket(AF_INET, SOCK_DGRAM, 0);
        udp_srv.sin_family = AF_INET;
        udp_srv.sin_port = htons(UDP_PORT);
        udp_srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
}

static void request(const char *mode, int32_t seq) {
    int32_t r;
    if (coid != -1) {
        MsgSend(coid, &seq, sizeof seq, &r, sizeof r);
    } else if (usock != -1) {
        sendto(usock, &seq, sizeof seq, 0, (struct sockaddr *)&udp_srv, sizeof udp_srv);
        recv(usock, &r, sizeof r, 0);
    } else {
        S->slot = seq;
        sem_post(&S->req);
        while (sem_wait(&S->rep) == -1 && errno == EINTR) {}
    }
    (void)mode;
}

/* ---- analysis ---- */
struct req_rec {
    uint64_t send, begin, end, reply;
    int srv_prio;
};

static int in_burst(const struct event *ev, unsigned n, uint64_t t, uint64_t *burst_end) {
    uint64_t open = 0;
    int is_open = 0;
    for (unsigned i = 0; i < n; i++) {
        if (ev[i].kind == EV_BURST_BEGIN && ev[i].t <= t) {
            open = ev[i].t;
            is_open = 1;
        } else if (ev[i].kind == EV_BURST_END && is_open && ev[i].t >= open) {
            if (ev[i].t <= t) {
                is_open = 0;
            } else {
                *burst_end = ev[i].t;
                return 1;
            }
        }
    }
    return 0;
}

static int cmp_ev(const void *a, const void *b) {
    const struct event *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: pinv msg|udp|shm [-n REQUESTS] [-l 0|1] [-v] [-m MARKER_DEVICE]\n");
        return 64;
    }
    const char *mode = argv[1];
    int nreq = 200, with_load = 1, verbose = 0;
    const char *marker = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) nreq = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) with_load = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) marker = argv[++i];
    }
    if (strcmp(mode, "msg") && strcmp(mode, "udp") && strcmp(mode, "shm")) {
        fprintf(stderr, "pinv: unknown mode %s\n", mode);
        return 64;
    }
    if (nreq < 1 || nreq > 5000) nreq = 200;
    cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    S = mmap(NULL, sizeof *S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, NOFD, 0);
    if (S == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(S, 0, sizeof *S);
    sem_init(&S->req, 1, 0);
    sem_init(&S->rep, 1, 0);

    become(PRIO_CLIENT);
    calibrate();

    pid_t srv = fork();
    if (srv == 0) {
        become(PRIO_SERVER);
        if (!strcmp(mode, "msg")) serve_msg();
        else if (!strcmp(mode, "udp")) serve_udp();
        else serve_shm();
        exit(0);
    }
    while (!S->server_ready) usleep(1000);
    usleep(20000);
    client_open(mode);

    pid_t ld = -1;
    if (with_load) {
        ld = fork();
        if (ld == 0) run_load();
        usleep(5000);
    }

    int mfd = marker ? open(marker, O_WRONLY) : -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    for (int32_t seq = 0; seq < nreq; seq++) {
        log_ev(EV_SEND, seq);
        if (mfd != -1) write(mfd, "S", 1);
        request(mode, seq);
        log_ev(EV_REPLY, seq);
        if (mfd != -1) write(mfd, "R", 1);
        sleep_until_ns(&ts, PERIOD_US * 1000L);
    }
    if (mfd != -1) {
        write(mfd, "\n", 1);
        close(mfd);
    }
    S->stop = 1;
    request(mode, -1);
    if (ld > 0) waitpid(ld, NULL, 0);
    waitpid(srv, NULL, 0);

    /* Analysis, after every competitor has exited. */
    unsigned n = atomic_load(&S->n);
    if (n > MAX_EVENTS) n = MAX_EVENTS;
    qsort(S->ev, n, sizeof S->ev[0], cmp_ev);
    struct req_rec *r = calloc((size_t)nreq, sizeof *r);
    for (unsigned i = 0; i < n; i++) {
        struct event *e = &S->ev[i];
        if (e->seq < 0 || e->seq >= nreq) continue;
        switch (e->kind) {
        case EV_SEND: r[e->seq].send = e->t; break;
        case EV_SRV_BEGIN:
            r[e->seq].begin = e->t;
            r[e->seq].srv_prio = e->prio;
            break;
        case EV_SRV_END: r[e->seq].end = e->t; break;
        case EV_REPLY: r[e->seq].reply = e->t; break;
        default: break;
        }
    }
    int misses = 0, contended = 0, server_first = 0, load_first = 0, at_client_prio = 0;
    uint64_t worst = 0;
    for (int i = 0; i < nreq; i++) {
        uint64_t rt = r[i].reply - r[i].send;
        if (rt > worst) worst = rt;
        int miss = rt > us2cyc(DEADLINE_US);
        misses += miss;
        if (r[i].srv_prio == PRIO_CLIENT) at_client_prio++;
        uint64_t bend = 0;
        int c = in_burst(S->ev, n, r[i].send, &bend);
        const char *who = "-";
        if (c) {
            contended++;
            /* The load was mid-burst when the request was sent: did the server finish before the burst ended? */
            if (r[i].end < bend) {
                server_first++;
                who = "server";
            } else {
                load_first++;
                who = "load";
            }
        }
        if (verbose)
            printf("req %4d prio %2d response_us %7.1f contended %d ran_first %-6s %s\n", i, r[i].srv_prio,
                   (double)rt * 1e6 / (double)cps, c, who, miss ? "MISS" : "ok");
    }
    printf("RESULT mode=%s load=%d requests=%d deadline_us=%d misses=%d contended=%d server_ran_first=%d "
           "load_ran_first=%d served_at_client_prio=%d worst_us=%.1f\n",
           mode, with_load, nreq, DEADLINE_US, misses, contended, server_first, load_first, at_client_prio,
           (double)worst * 1e6 / (double)cps);
    return 0;
}
