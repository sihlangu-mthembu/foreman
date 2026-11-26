/*
 * Foreman shared-memory layout.
 *
 * Two POSIX shared-memory objects connect the processes:
 *   /foreman_state  mode 0644  written only by fm-plant, read by everyone (telemetry, agent, dashboard)
 *   /foreman_ctl    mode 0600  operator commands (fault injection, stats reset); root-only by design
 *
 * The AI agent can map /foreman_state read-only and cannot open /foreman_ctl at all: it can see the
 * machine but has no path to change it.
 *
 * Readers use a sequence lock: the writer makes `seq` odd while updating and even when done; a reader
 * copies the block and retries if `seq` changed or was odd.
 */
#ifndef FM_SHM_H
#define FM_SHM_H

#include <stdint.h>
#include <string.h>

#define FM_STATE_SHM "/foreman_state"
#define FM_CTL_SHM   "/foreman_ctl"
#define FM_MAGIC     0x464d4e31u /* "FMN1" */
#define FM_VERSION   1

#define FM_NSIG      6      /* signals per profile */
#define FM_TREND_N   3600   /* 1 Hz trend history: 60 minutes */
#define FM_FAULT_N   64     /* fault/event log ring */
#define FM_HIST_N    100    /* wake-latency histogram bins */
#define FM_HIST_US   10     /* bin width in microseconds -> covers 0..1 ms, last bin is overflow */

enum fm_sig { FM_SP = 0, FM_PV, FM_OUT, FM_AUX1, FM_AUX2, FM_CURRENT };

enum fm_mode { FM_RUN = 0, FM_DERATED = 1, FM_STOPPED = 2 };

struct fm_signal_info {
    char name[32];
    char unit[12];
};

struct fm_sample {
    uint64_t t_ms;            /* ms since plant start */
    float v[FM_NSIG];
    uint8_t mode;             /* enum fm_mode */
    uint8_t n_active;         /* active alarms at this instant */
    uint16_t pad;
};

struct fm_event {
    uint64_t t_ms;
    char code[8];
    char text[88];
    float value;              /* signal value that tripped the rule */
    uint8_t kind;             /* 0 = alarm raised, 1 = alarm cleared, 2 = operator action */
    uint8_t pad[3];
};

struct fm_timing {
    uint64_t cycles;
    uint64_t misses;          /* cycles whose work finished after the next period began */
    uint64_t hist[FM_HIST_N]; /* wake latency, FM_HIST_US-wide bins */
    float max_latency_us;
    float mean_latency_us;
    float max_exec_us;        /* control computation time per cycle */
    uint32_t period_us;
    uint64_t since_ms;        /* stats window start (reset by operator) */
};

struct fm_state {
    uint32_t magic, version;
    volatile uint32_t seq;
    uint32_t pid;
    char profile[24];
    char machine[64];
    struct fm_signal_info sig[FM_NSIG];
    struct fm_sample live;
    struct fm_timing timing;
    uint32_t active_mask;     /* bit i set: rule i alarm active */
    char active_codes[8][8];
    uint32_t n_active;
    char scenario[32];        /* injected scenario currently running, "" if none */
    uint32_t trend_head;      /* next write index */
    uint32_t trend_count;
    struct fm_sample trend[FM_TREND_N];
    uint32_t ev_head, ev_count;
    struct fm_event ev[FM_FAULT_N];
};

#define FM_CTL_Q 16
struct fm_ctl {               /* single-producer command queue: fmctl writes slot head%Q, then bumps head */
    volatile uint32_t head;
    char cmd[FM_CTL_Q][120];  /* "inject <scenario>", "clear", "reset-stats", "setpoint <value>" */
};

/* --- seqlock helpers --- */
static inline void fm_write_begin(struct fm_state *s) { s->seq++; __atomic_thread_fence(__ATOMIC_RELEASE); }
static inline void fm_write_end(struct fm_state *s)   { __atomic_thread_fence(__ATOMIC_RELEASE); s->seq++; }

/* Copy `len` bytes at offset `off` of the shared block into dst, consistently. Returns 0 on success. */
static inline int fm_read(const struct fm_state *s, void *dst, size_t off, size_t len) {
    for (int tries = 0; tries < 1000; tries++) {
        uint32_t a = s->seq;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (a & 1u) continue;
        memcpy(dst, (const char *)s + off, len);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (s->seq == a) return 0;
    }
    return -1;
}

#endif
