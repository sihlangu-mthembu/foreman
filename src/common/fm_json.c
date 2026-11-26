#define _POSIX_C_SOURCE 200809L
#include "fm_json.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *mode_name(int m) { return m == FM_STOPPED ? "stopped" : m == FM_DERATED ? "derated" : "running"; }

const struct fm_state *fm_attach(void) {
    int fd = shm_open(FM_STATE_SHM, O_RDONLY, 0);
    if (fd < 0) return NULL;
    void *p = mmap(NULL, sizeof(struct fm_state), PROT_READ, MAP_SHARED, fd, 0); /* read-only mapping */
    close(fd);
    if (p == MAP_FAILED) return NULL;
    const struct fm_state *st = p;
    return st->magic == FM_MAGIC ? st : NULL;
}

struct sb { char *b; size_t n, len; int err; };
static void put(struct sb *s, const char *fmt, ...) {
    if (s->err) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(s->b + s->len, s->n - s->len, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= s->n - s->len) { s->err = 1; return; }
    s->len += (size_t)k;
}
static void put_str(struct sb *s, const char *str) { /* JSON-escape a short string */
    put(s, "\"");
    for (; *str && !s->err; str++) {
        unsigned char c = (unsigned char)*str;
        if (c == '"' || c == '\\') put(s, "\\%c", c);
        else if (c < 0x20) put(s, "\\u%04x", c);
        else put(s, "%c", c);
    }
    put(s, "\"");
}

/* Snapshot of the header part (everything up to the trend array) */
#define HDR_LEN offsetof(struct fm_state, trend)

int fm_json_status(const struct fm_state *st, char *buf, size_t n) {
    static struct fm_state h; /* header snapshot only */
    if (fm_read(st, &h, 0, HDR_LEN)) return -1;
    struct sb s = {buf, n, 0, 0};
    put(&s, "{\"machine\":"); put_str(&s, h.machine);
    put(&s, ",\"profile\":"); put_str(&s, h.profile);
    put(&s, ",\"uptime_s\":%.1f,\"mode\":\"%s\",\"values\":{", h.live.t_ms / 1000.0, mode_name(h.live.mode));
    for (int i = 0; i < FM_NSIG; i++) {
        put(&s, "%s", i ? "," : ""); put_str(&s, h.sig[i].name);
        put(&s, ":{\"value\":%.2f,\"unit\":", h.live.v[i]); put_str(&s, h.sig[i].unit); put(&s, "}");
    }
    put(&s, "},\"active_alarms\":[");
    for (uint32_t i = 0; i < h.n_active && i < 8; i++) { put(&s, "%s", i ? "," : ""); put_str(&s, h.active_codes[i]); }
    put(&s, "],\"injected_scenario\":"); put_str(&s, h.scenario);
    put(&s, "}");
    return s.err ? -1 : (int)s.len;
}

int fm_json_signals(const struct fm_state *st, char *buf, size_t n) {
    static struct fm_state h;
    if (fm_read(st, &h, 0, HDR_LEN)) return -1;
    struct sb s = {buf, n, 0, 0};
    put(&s, "[");
    for (int i = 0; i < FM_NSIG; i++) {
        put(&s, "%s{\"index\":%d,\"name\":", i ? "," : "", i); put_str(&s, h.sig[i].name);
        put(&s, ",\"unit\":"); put_str(&s, h.sig[i].unit); put(&s, "}");
    }
    put(&s, "]");
    return s.err ? -1 : (int)s.len;
}

int fm_json_events(const struct fm_state *st, char *buf, size_t n) {
    static struct fm_event ev[FM_FAULT_N];
    uint32_t head, count;
    static struct fm_state h;
    for (int tries = 0; tries < 100; tries++) {
        uint32_t a = st->seq;
        if (a & 1u) continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        head = st->ev_head; count = st->ev_count;
        memcpy(ev, st->ev, sizeof ev);
        memcpy(&h.live, &st->live, sizeof h.live);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (st->seq == a) goto ok;
    }
    return -1;
ok:;
    struct sb s = {buf, n, 0, 0};
    put(&s, "{\"now_s\":%.1f,\"events\":[", h.live.t_ms / 1000.0);
    for (uint32_t i = 0; i < count; i++) { /* oldest first */
        const struct fm_event *e = &ev[(head + FM_FAULT_N - count + i) % FM_FAULT_N];
        static const char *kinds[] = {"alarm", "cleared", "operator"};
        put(&s, "%s{\"t_s\":%.1f,\"kind\":\"%s\",\"code\":", i ? "," : "", e->t_ms / 1000.0, kinds[e->kind % 3]);
        put_str(&s, e->code); put(&s, ",\"text\":"); put_str(&s, e->text);
        put(&s, ",\"value\":%.2f}", e->value);
    }
    put(&s, "]}");
    return s.err ? -1 : (int)s.len;
}

int fm_json_timing(const struct fm_state *st, char *buf, size_t n) {
    static struct fm_state h;
    if (fm_read(st, &h, 0, HDR_LEN)) return -1;
    const struct fm_timing *t = &h.timing;
    uint64_t acc = 0, p99 = 0, p999 = 0;
    for (int i = 0; i < FM_HIST_N; i++) {
        acc += t->hist[i];
        if (!p99 && acc * 100 >= t->cycles * 99) p99 = (uint64_t)(i + 1) * FM_HIST_US;
        if (!p999 && acc * 1000 >= t->cycles * 999) p999 = (uint64_t)(i + 1) * FM_HIST_US;
    }
    struct sb s = {buf, n, 0, 0};
    put(&s, "{\"period_us\":%u,\"cycles\":%llu,\"deadline_misses\":%llu,\"mean_wake_latency_us\":%.2f,"
            "\"p99_wake_latency_us\":%llu,\"p999_wake_latency_us\":%llu,\"max_wake_latency_us\":%.1f,"
            "\"max_exec_us\":%.1f,\"window_s\":%.1f,\"hist_bin_us\":%d,\"hist\":[",
        t->period_us, (unsigned long long)t->cycles, (unsigned long long)t->misses, t->mean_latency_us,
        (unsigned long long)p99, (unsigned long long)p999, t->max_latency_us, t->max_exec_us,
        (h.live.t_ms - t->since_ms) / 1000.0, FM_HIST_US);
    int last = 0;
    for (int i = 0; i < FM_HIST_N; i++) if (t->hist[i]) last = i;
    for (int i = 0; i <= last; i++) put(&s, "%s%llu", i ? "," : "", (unsigned long long)t->hist[i]);
    put(&s, "]}");
    return s.err ? -1 : (int)s.len;
}

int fm_json_trend(const struct fm_state *st, int sig, int minutes, int step_s, char *buf, size_t n) {
    static struct fm_sample tr[FM_TREND_N];
    static struct fm_signal_info info[FM_NSIG];
    uint32_t head = 0, count = 0;
    if (sig < 0 || sig >= FM_NSIG) return -1;
    if (minutes < 1) minutes = 1;
    if (minutes > 60) minutes = 60;
    if (step_s < 1) step_s = 1;
    int ok = 0;
    for (int tries = 0; tries < 100 && !ok; tries++) {
        uint32_t a = st->seq;
        if (a & 1u) continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        head = st->trend_head; count = st->trend_count;
        memcpy(tr, st->trend, sizeof tr);
        memcpy(info, st->sig, sizeof info);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        ok = st->seq == a;
    }
    if (!ok) return -1;
    uint32_t want = (uint32_t)minutes * 60u;
    if (want > count) want = count;
    float mn = 1e30f, mx = -1e30f, sum = 0, first = 0, lastv = 0;
    struct sb s = {buf, n, 0, 0};
    put(&s, "{\"signal\":"); put_str(&s, info[sig].name); put(&s, ",\"unit\":"); put_str(&s, info[sig].unit);
    put(&s, ",\"samples\":[");
    int k = 0;
    for (uint32_t i = 0; i < want; i++) {
        const struct fm_sample *x = &tr[(head + FM_TREND_N - want + i) % FM_TREND_N];
        float v = x->v[sig];
        if (i == 0) first = v;
        lastv = v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
        if (i % (uint32_t)step_s == 0 || i + 1 == want)
            put(&s, "%s[%.0f,%.2f]", k++ ? "," : "", x->t_ms / 1000.0, v);
    }
    put(&s, "],\"summary\":{\"minutes\":%d,\"n\":%u,\"min\":%.2f,\"max\":%.2f,\"mean\":%.2f,\"first\":%.2f,\"last\":%.2f,\"change\":%.2f}}",
        minutes, want, want ? mn : 0, want ? mx : 0, want ? sum / (float)want : 0, first, lastv, lastv - first);
    return s.err ? -1 : (int)s.len;
}
