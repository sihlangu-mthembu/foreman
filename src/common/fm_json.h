/* Read-only views of /foreman_state rendered as JSON. Used by the telemetry service. */
#ifndef FM_JSON_H
#define FM_JSON_H
#include <stddef.h>
#include "fm_shm.h"

/* Map /foreman_state read-only. Returns NULL if the plant is not running. */
const struct fm_state *fm_attach(void);

/* Each writer fills buf (NUL-terminated) and returns the length, or -1 if the snapshot failed. */
int fm_json_status(const struct fm_state *st, char *buf, size_t n);
int fm_json_events(const struct fm_state *st, char *buf, size_t n);
int fm_json_timing(const struct fm_state *st, char *buf, size_t n);
/* sig: index 0..5, minutes: how far back (1..60), step_s: sample spacing in seconds */
int fm_json_trend(const struct fm_state *st, int sig, int minutes, int step_s, char *buf, size_t n);
int fm_json_signals(const struct fm_state *st, char *buf, size_t n);

#endif
