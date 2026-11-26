/*
 * fm-telemetryd: a QNX resource manager that publishes the machine's state as read-only files.
 *
 *   /dev/foreman/status          live values, mode, active alarms
 *   /dev/foreman/events          alarm and operator event log
 *   /dev/foreman/timing          control-loop timing statistics (wake latency histogram, deadline misses)
 *   /dev/foreman/signals         signal names and units for this machine
 *   /dev/foreman/trend/<i>[/<minutes>]   1 Hz history of signal i (default 10 minutes)
 *
 * Every file is a JSON snapshot taken when it is opened. Opening for write fails with EROFS and write()
 * is refused, so a client of this service, such as the AI agent, has read access only. Because QNX
 * message passing transfers the client's priority to the server thread that handles the request, a
 * low-priority AI client is also served at low priority and cannot push this work ahead of the
 * control loop.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fm_ocb;
#define IOFUNC_OCB_T struct fm_ocb
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "../common/fm_json.h"

struct fm_ocb {
    iofunc_ocb_t hdr; /* must be first */
    char *data;
    size_t len;
};

static const struct fm_state *g_st;
static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static char g_buf[1 << 18];

/* Render the file named by `path` (relative to /dev/foreman) into a fresh heap buffer. */
static int render(const char *path, char **out, size_t *len) {
    int n = -1, sig, minutes = 10;
    if (!g_st && !(g_st = fm_attach())) return ENODEV; /* plant not running yet */
    if (strcmp(path, "status") == 0) n = fm_json_status(g_st, g_buf, sizeof g_buf);
    else if (strcmp(path, "events") == 0) n = fm_json_events(g_st, g_buf, sizeof g_buf);
    else if (strcmp(path, "timing") == 0) n = fm_json_timing(g_st, g_buf, sizeof g_buf);
    else if (strcmp(path, "signals") == 0) n = fm_json_signals(g_st, g_buf, sizeof g_buf);
    else if (sscanf(path, "trend/%d/%d", &sig, &minutes) >= 1) {
        if (sig < 0 || sig >= FM_NSIG) return ENOENT;
        n = fm_json_trend(g_st, sig, minutes, minutes > 10 ? 10 : 5, g_buf, sizeof g_buf);
    } else return ENOENT;
    if (n < 0) return EAGAIN;
    *out = malloc((size_t)n + 1);
    if (!*out) return ENOMEM;
    memcpy(*out, g_buf, (size_t)n);
    (*out)[n] = '\n';
    *len = (size_t)n + 1;
    return EOK;
}

static int fm_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra) {
    (void)handle;
    if (msg->connect.ioflag & _IO_FLAG_WR) return EROFS;          /* read-only by design */
    if (msg->connect.ioflag & (O_CREAT | O_TRUNC)) return EROFS;
    struct fm_ocb *ocb = calloc(1, sizeof *ocb);
    if (!ocb) return ENOMEM;
    int status = render(msg->connect.path, &ocb->data, &ocb->len);
    if (status != EOK) { free(ocb); return status; }
    if ((status = iofunc_open(ctp, msg, &g_attr, NULL, NULL)) != EOK ||
        (status = iofunc_ocb_attach(ctp, msg, ocb, &g_attr, NULL)) != EOK) {
        free(ocb->data); free(ocb);
        return status;
    }
    (void)extra;
    return EOK;
}

static int fm_read(resmgr_context_t *ctp, io_read_t *msg, struct fm_ocb *ocb) {
    int status = iofunc_read_verify(ctp, msg, &ocb->hdr, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    size_t off = (size_t)ocb->hdr.offset;
    size_t n = off < ocb->len ? ocb->len - off : 0;
    if (n > msg->i.nbytes) n = msg->i.nbytes;
    _IO_SET_READ_NBYTES(ctp, n);
    if (n == 0) return _RESMGR_NPARTS(0);
    ocb->hdr.offset += n;
    SETIOV(ctp->iov, ocb->data + off, n);
    return _RESMGR_NPARTS(1);
}

static int fm_write(resmgr_context_t *ctp, io_write_t *msg, struct fm_ocb *ocb) {
    (void)ctp; (void)msg; (void)ocb;
    return EROFS;
}

static int fm_close(resmgr_context_t *ctp, void *reserved, struct fm_ocb *ocb) {
    free(ocb->data);
    ocb->data = NULL;
    return iofunc_close_ocb_default(ctp, reserved, &ocb->hdr);
}

int main(int argc, char **argv) {
    const char *mount = argc > 1 ? argv[1] : "/dev/foreman";
    dispatch_t *dpp = dispatch_create();
    if (!dpp) { perror("dispatch_create"); return 1; }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = fm_open;
    g_io.read = fm_read;
    g_io.write = fm_write;
    g_io.close_ocb = fm_close;
    iofunc_attr_init(&g_attr, S_IFDIR | 0555, NULL, NULL);

    resmgr_attr_t rattr = {.nparts_max = 1, .msg_max_size = 2048};
    if (resmgr_attach(dpp, &rattr, mount, _FTYPE_ANY, _RESMGR_FLAG_DIR, &g_connect, &g_io, &g_attr) == -1) {
        perror("resmgr_attach");
        return 1;
    }
    /* Pathname is registered: privileges for that are no longer needed. */
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    fprintf(stderr, "fm-telemetryd: serving %s (read-only)\n", mount);
    for (;;) {
        if (!(ctp = dispatch_block(ctp))) { perror("dispatch_block"); return 1; }
        dispatch_handler(ctp);
    }
}
