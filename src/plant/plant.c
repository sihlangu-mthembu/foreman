/*
 * fm-plant: the safety-critical side of Foreman.
 *
 * Runs a simulated machine (see profiles.c) under a 1 kHz closed control loop, detects faults with
 * hold-time rules, and publishes live values, a 1 Hz trend, an event log and its own timing statistics
 * to /foreman_state. Operator commands (fault injection for demos, alarm reset) arrive through
 * /foreman_ctl, which only root can open.
 *
 * On QNX the period comes from a timer that delivers a pulse to a private channel, and the loop thread
 * runs at a fixed SCHED_FIFO priority above everything the AI uses. Elsewhere (for development on Linux)
 * it falls back to clock_nanosleep with an absolute deadline.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef __QNX__
#define _QNX_SOURCE /* QNX extensions: pulses, timers, ThreadCtl */
#endif
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __QNX__
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#endif

#include "profiles.h"

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 1)

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Small deterministic noise so trends look like real sensors. */
static uint32_t g_rng = 0x12345678u;
static float noise(float amp) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return amp * ((float)(g_rng & 0xffff) / 32768.0f - 1.0f);
}

struct rule_state { int active; int held_ms; int clear_ms; };

struct sim {
    const struct fm_profile *p;
    float sp, pv, pv_est, out, integ, aux1, aux2, current, bias;
    float load_extra, aux1_extra;       /* current (ramped) scenario effects */
    const struct fm_scenario *scen;
    float scen_t;
    int mode;
    float derate;
    struct rule_state rs[16];
};

static struct fm_state *g_st;
static struct fm_ctl *g_ctl;
static uint32_t g_ctl_seq;
static uint64_t g_t0_ns;

static uint64_t t_ms(void) { return (now_ns() - g_t0_ns) / 1000000ull; }

static void log_event(int kind, const char *code, const char *text, float value) {
    struct fm_event *e = &g_st->ev[g_st->ev_head];
    e->t_ms = t_ms();
    e->kind = (uint8_t)kind;
    e->value = value;
    snprintf(e->code, sizeof e->code, "%s", code);
    snprintf(e->text, sizeof e->text, "%s", text);
    g_st->ev_head = (g_st->ev_head + 1) % FM_FAULT_N;
    if (g_st->ev_count < FM_FAULT_N) g_st->ev_count++;
}

static float sig_value(const struct sim *s, int sig, const float *v) {
    if (sig == FM_SIG_EST_DEV) return fabsf(s->pv_est - v[FM_PV]);
    if (sig == FM_SIG_TRACK) return fabsf(v[FM_SP] - v[FM_PV]);
    return v[sig];
}

/* One control period: plant physics, PI control, fault rules. dt in seconds. */
static void step(struct sim *s, float dt, float *v) {
    const struct fm_profile *p = s->p;

    /* Scenario effects ramp in over ramp_s seconds. */
    if (s->scen) {
        s->scen_t += dt;
        float k = s->scen->ramp_s > 0 ? fminf(1.0f, s->scen_t / s->scen->ramp_s) : 1.0f;
        s->load_extra = k * s->scen->load_extra;
        s->aux1_extra = k * s->scen->aux1_extra;
        s->bias += s->scen->bias_per_s * dt;
    }

    float load = (p->aux2_kind == AUX2_LOAD ? p->aux2_init : 0.0f) + s->load_extra;
    float sp_eff = s->sp * (s->mode == FM_DERATED ? s->derate : 1.0f);
    float meas = s->pv + s->bias + noise(0.002f * p->sp);
    if (meas < 0) meas = 0;

    /* PI controller on the measured value; anti-windup by clamping the integrator. */
    if (s->mode == FM_STOPPED) {
        s->out = 0; s->integ = 0;
    } else {
        float err = sp_eff - meas;
        s->integ += p->ki * err * dt;
        if (s->integ > 100) s->integ = 100;
        if (s->integ < 0) s->integ = 0;
        s->out = p->kp * err + s->integ;
        if (s->out > 100) s->out = 100;
        if (s->out < 0) s->out = 0;
    }

    /* First-order plant; pv_est is the drive's own model of the same plant (no sensor in the loop). */
    float target = p->gain * s->out - p->load_coef * load;
    if (target < 0) target = 0;
    s->pv += (target - s->pv) * dt / p->tau_pv;
    s->pv_est += (target - s->pv_est) * dt / p->tau_pv;

    s->current = p->cur_base + p->cur_gain * s->out * (1.0f + load / 100.0f);
    float aux1_target = p->aux1_base + p->aux1_k * s->current + s->aux1_extra;
    s->aux1 += (aux1_target - s->aux1) * dt / p->tau_aux1;

    switch (p->aux2_kind) {
    case AUX2_LOAD: s->aux2 = load; break;
    case AUX2_DRAIN: s->aux2 -= p->aux2_rate * s->current * dt; if (s->aux2 < 0) s->aux2 = 0; break;
    case AUX2_INTEGRAL: s->aux2 += s->pv * p->aux2_rate * dt; break;
    }

    v[FM_SP] = sp_eff;
    v[FM_PV] = meas;
    v[FM_OUT] = s->out;
    v[FM_AUX1] = s->aux1 + noise(0.002f * p->aux1_base);
    v[FM_AUX2] = s->aux2;
    v[FM_CURRENT] = s->current + noise(0.01f * p->cur_base);

    /* Rules: raise after hold_ms; warnings and derates clear after 5 s back inside a hysteresis band;
       stops latch until an operator clears them. */
    int dms = (int)(dt * 1000.0f + 0.5f);
    for (int i = 0; i < p->n_rules && i < 16; i++) {
        const struct fm_rule *r = &p->rules[i];
        struct rule_state *rs = &s->rs[i];
        float x = sig_value(s, r->sig, v);
        int trip = r->op == '>' ? x > r->threshold : x < r->threshold;
        float hyst = 0.08f * fabsf(r->threshold);
        int inside = r->op == '>' ? x < r->threshold - hyst : x > r->threshold + hyst;
        if (!rs->active) {
            rs->held_ms = trip ? rs->held_ms + dms : 0;
            if (rs->held_ms >= r->hold_ms) {
                rs->active = 1; rs->clear_ms = 0;
                log_event(0, r->code, r->title, x);
                if (r->action == ACT_STOP) s->mode = FM_STOPPED;
                else if (r->action == ACT_DERATE && s->mode != FM_STOPPED) { s->mode = FM_DERATED; s->derate = r->derate; }
            }
        } else if (r->action != ACT_STOP) {
            rs->clear_ms = inside ? rs->clear_ms + dms : 0;
            if (rs->clear_ms >= 5000) {
                rs->active = 0; rs->held_ms = 0;
                log_event(1, r->code, "cleared", x);
                if (r->action == ACT_DERATE && s->mode == FM_DERATED) s->mode = FM_RUN;
            }
        }
    }
}

static void publish_active(const struct sim *s) {
    g_st->n_active = 0;
    g_st->active_mask = 0;
    for (int i = 0; i < s->p->n_rules && i < 16; i++) {
        if (!s->rs[i].active) continue;
        g_st->active_mask |= 1u << i;
        if (g_st->n_active < 8) snprintf(g_st->active_codes[g_st->n_active++], 8, "%s", s->p->rules[i].code);
    }
}

static void handle_command(struct sim *s, const char *cmd) {
    char arg[100] = "";
    if (sscanf(cmd, "inject %99s", arg) == 1) {
        for (int i = 0; i < s->p->n_scenarios; i++) {
            if (strcmp(s->p->scenarios[i].name, arg) != 0) continue;
            s->scen = &s->p->scenarios[i];
            s->scen_t = 0;
            if (s->scen->aux2_set >= 0) s->aux2 = s->scen->aux2_set;
            snprintf(g_st->scenario, sizeof g_st->scenario, "%s", arg);
            log_event(2, "OP", s->scen->description, 0);
            return;
        }
        log_event(2, "OP", "unknown scenario requested", 0);
    } else if (strcmp(cmd, "clear") == 0) {
        s->scen = NULL; s->load_extra = s->aux1_extra = 0; s->bias = 0; s->scen_t = 0;
        if (s->p->aux2_kind == AUX2_DRAIN && s->aux2 < 20) s->aux2 = s->p->aux2_init; /* "recharged" */
        memset(s->rs, 0, sizeof s->rs);
        s->mode = FM_RUN; s->derate = 1;
        g_st->scenario[0] = 0;
        log_event(2, "OP", "operator cleared alarms and scenario; machine restarted", 0);
    } else if (strcmp(cmd, "reset-stats") == 0) {
        memset(&g_st->timing.hist, 0, sizeof g_st->timing.hist);
        g_st->timing.cycles = g_st->timing.misses = 0;
        g_st->timing.max_latency_us = g_st->timing.mean_latency_us = g_st->timing.max_exec_us = 0;
        g_st->timing.since_ms = t_ms();
    } else if (sscanf(cmd, "setpoint %99s", arg) == 1) {
        s->sp = strtof(arg, NULL);
        log_event(2, "OP", "operator changed setpoint", s->sp);
    }
}

static void *map_shm(const char *name, size_t size, mode_t mode) {
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, mode);
    if (fd < 0) { perror(name); return NULL; }
    fchmod(fd, mode); /* umask must not widen or narrow the intended access */
    if (ftruncate(fd, (off_t)size) != 0) { perror("ftruncate"); close(fd); return NULL; }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static void set_rt(int prio, int cpu) {
    struct sched_param sp = {.sched_priority = prio};
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc) fprintf(stderr, "fm-plant: SCHED_FIFO %d not set (%s); timing will not be guaranteed\n", prio, strerror(rc));
#ifdef __QNX__
    if (cpu >= 0 && ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) perror("runmask");
#else
    (void)cpu;
#endif
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) perror("mlockall (continuing)");
}

static void usage(void) {
    fprintf(stderr,
            "usage: fm-plant [-p profile] [-P priority] [-c cpu] [-d seconds] [-r]\n"
            "  -p  machine profile: %s (default industrial)\n"
            "  -P  SCHED_FIFO priority of the control loop (default 70: above the 1-63 range open to unprivileged processes)\n"
            "  -c  pin the loop to one CPU (default: any)\n"
            "  -d  run for N seconds then exit (benchmarks)\n"
            "  -r  print a JSON timing report on exit\n",
            fm_profile_list());
}

int main(int argc, char **argv) {
    const char *pname = "industrial";
    int prio = 70, cpu = -1, report = 0;
    double duration = 0;
    int c;
    while ((c = getopt(argc, argv, "p:P:c:d:rh")) != -1) {
        switch (c) {
        case 'p': pname = optarg; break;
        case 'P': prio = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'r': report = 1; break;
        default: usage(); return c == 'h' ? 0 : 2;
        }
    }
    const struct fm_profile *p = fm_profile_find(pname);
    if (!p) { fprintf(stderr, "unknown profile '%s' (have: %s)\n", pname, fm_profile_list()); return 2; }

    g_st = map_shm(FM_STATE_SHM, sizeof *g_st, 0644);
    g_ctl = map_shm(FM_CTL_SHM, sizeof *g_ctl, 0600);
    if (!g_st || !g_ctl) return 1;
    memset(g_st, 0, sizeof *g_st);
    g_st->magic = FM_MAGIC; g_st->version = FM_VERSION; g_st->pid = (uint32_t)getpid();
    snprintf(g_st->profile, sizeof g_st->profile, "%s", p->name);
    snprintf(g_st->machine, sizeof g_st->machine, "%s", p->machine);
    memcpy(g_st->sig, p->sig, sizeof g_st->sig);

    struct sim s = {0};
    s.p = p; s.sp = p->sp; s.derate = 1; s.aux2 = p->aux2_init; s.aux1 = p->aux1_base;
    /* Start at steady state so the first minute of trend is meaningful. */
    for (int i = 0; i < 60000; i++) { float v[FM_NSIG]; step(&s, 0.001f, v); }
    memset(s.rs, 0, sizeof s.rs);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    set_rt(prio, cpu);

    const uint32_t period_us = 1000;
    const uint64_t period_ns = period_us * 1000ull;
    g_st->timing.period_us = period_us;
    g_t0_ns = now_ns();
    uint64_t next = g_t0_ns + period_ns;

#ifdef __QNX__
    /* Periodic timer delivering a pulse to a private channel: the classic QNX real-time loop. */
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE_TICK, 0);
    timer_t tid;
    if (chid < 0 || coid < 0 || timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0) { perror("timer"); return 1; }
    struct itimerspec its = {
        .it_value = {.tv_sec = (time_t)(next / 1000000000ull), .tv_nsec = (long)(next % 1000000000ull)},
        .it_interval = {.tv_sec = 0, .tv_nsec = (long)period_ns},
    };
    if (timer_settime(tid, TIMER_ABSTIME, &its, NULL) != 0) { perror("timer_settime"); return 1; }
#endif

    double lat_sum = 0;
    float v[FM_NSIG];
    uint64_t end_ns = duration > 0 ? g_t0_ns + (uint64_t)(duration * 1e9) : 0;

    while (!g_stop) {
#ifdef __QNX__
        struct _pulse pulse;
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) { if (errno == EINTR) continue; perror("MsgReceivePulse"); break; }
#else
        struct timespec ts = {.tv_sec = (time_t)(next / 1000000000ull), .tv_nsec = (long)(next % 1000000000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_stop) {}
#endif
        uint64_t woke = now_ns();
        static int anchored = 0;
        if (!anchored) {
            /* The periodic timer expires on the system clock tick, so its phase relative to our start time is
               arbitrary. Anchor the schedule to the first expiry: latency then measures true jitter. */
            next = woke; anchored = 1;
        }
        float lat_us = woke > next ? (float)(woke - next) / 1000.0f : 0.0f;

        step(&s, period_us / 1e6f, v);

        struct fm_timing *tm = &g_st->timing;
        static int first = 1; /* the first wake after start-up is not representative */
        fm_write_begin(g_st);
        memcpy(g_st->live.v, v, sizeof v);
        g_st->live.t_ms = (woke - g_t0_ns) / 1000000ull;
        g_st->live.mode = (uint8_t)s.mode;
        publish_active(&s);
        g_st->live.n_active = (uint8_t)g_st->n_active;
        if (tm->cycles % 1000 == 0) {
            g_st->trend[g_st->trend_head] = g_st->live;
            g_st->trend_head = (g_st->trend_head + 1) % FM_TREND_N;
            if (g_st->trend_count < FM_TREND_N) g_st->trend_count++;
        }
        uint64_t done = now_ns();
        if (!first) {
            tm->cycles++;
            int bin = (int)(lat_us / FM_HIST_US);
            tm->hist[bin < FM_HIST_N ? bin : FM_HIST_N - 1]++;
            if (lat_us > tm->max_latency_us) tm->max_latency_us = lat_us;
            lat_sum += lat_us;
            tm->mean_latency_us = (float)(lat_sum / (double)tm->cycles);
            float exec_us = (float)(done - woke) / 1000.0f;
            if (exec_us > tm->max_exec_us) tm->max_exec_us = exec_us;
            if (done > next + period_ns) tm->misses++;
        }
        first = 0;
        fm_write_end(g_st);

        /* Operator commands, polled every 10 ms. */
        if (tm->cycles % 10 == 0 && g_ctl->seq != g_ctl_seq) {
            g_ctl_seq = g_ctl->seq;
            char cmd[sizeof g_ctl->cmd];
            memcpy(cmd, g_ctl->cmd, sizeof cmd);
            cmd[sizeof cmd - 1] = 0;
            fm_write_begin(g_st);
            handle_command(&s, cmd);
            fm_write_end(g_st);
            if (strcmp(cmd, "reset-stats") == 0) lat_sum = 0;
        }

        next += period_ns;
        if (woke > next) { /* fell behind by more than a period: count skipped deadlines, resync */
            uint64_t behind = (woke - next) / period_ns + 1;
            tm->misses += behind;
            next += behind * period_ns;
        }
        if (end_ns && woke >= end_ns) break;
    }

    if (report) {
        struct fm_timing *tm = &g_st->timing;
        uint64_t n = 0, p99 = 0, p999 = 0;
        for (int i = 0; i < FM_HIST_N; i++) {
            n += tm->hist[i];
            if (!p99 && n >= tm->cycles * 99 / 100) p99 = (uint64_t)(i + 1) * FM_HIST_US;
            if (!p999 && n >= tm->cycles * 999 / 1000) p999 = (uint64_t)(i + 1) * FM_HIST_US;
        }
        printf("{\"profile\":\"%s\",\"cycles\":%llu,\"misses\":%llu,\"mean_latency_us\":%.2f,"
               "\"p99_latency_us\":%llu,\"p999_latency_us\":%llu,\"max_latency_us\":%.1f,\"max_exec_us\":%.1f}\n",
               p->name, (unsigned long long)tm->cycles, (unsigned long long)tm->misses, tm->mean_latency_us,
               (unsigned long long)p99, (unsigned long long)p999, tm->max_latency_us, tm->max_exec_us);
    }
    shm_unlink(FM_CTL_SHM);
    shm_unlink(FM_STATE_SHM);
    return 0;
}
