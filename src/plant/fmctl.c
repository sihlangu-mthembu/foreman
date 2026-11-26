/*
 * fmctl: operator console for the simulated machine (root only, because /foreman_ctl is mode 0600).
 *   fmctl inject <scenario> | clear | reset-stats | setpoint <value>
 *   fmctl status | events | timing | trend <signal-index> [minutes]
 * The read commands need no privilege; they use the same read-only view the AI agent gets.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../common/fm_json.h"

static char buf[1 << 18];

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fmctl inject <scenario>|clear|reset-stats|setpoint <v>|status|events|timing|trend <sig> [min]\n");
        return 2;
    }
    const char *c = argv[1];
    if (!strcmp(c, "status") || !strcmp(c, "events") || !strcmp(c, "timing") || !strcmp(c, "trend")) {
        const struct fm_state *st = fm_attach();
        if (!st) { fprintf(stderr, "fmctl: plant not running\n"); return 1; }
        int n = !strcmp(c, "status") ? fm_json_status(st, buf, sizeof buf)
              : !strcmp(c, "events") ? fm_json_events(st, buf, sizeof buf)
              : !strcmp(c, "timing") ? fm_json_timing(st, buf, sizeof buf)
              : fm_json_trend(st, argc > 2 ? atoi(argv[2]) : 1, argc > 3 ? atoi(argv[3]) : 5, 10, buf, sizeof buf);
        if (n < 0) { fprintf(stderr, "fmctl: snapshot failed\n"); return 1; }
        puts(buf);
        return 0;
    }
    int fd = shm_open(FM_CTL_SHM, O_RDWR, 0);
    if (fd < 0) { perror("fmctl: open " FM_CTL_SHM " (operator commands need root)"); return 1; }
    struct fm_ctl *ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ctl == MAP_FAILED) { perror("mmap"); return 1; }
    char cmd[sizeof ctl->cmd] = "";
    for (int i = 1; i < argc; i++) {
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
        if (i + 1 < argc) strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
    }
    memcpy(ctl->cmd, cmd, sizeof cmd);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ctl->seq++;
    printf("sent: %s\n", cmd);
    return 0;
}
