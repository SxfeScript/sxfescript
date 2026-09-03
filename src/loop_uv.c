/* The libuv loop backend: what sxn has always done, behind the interface.
 *
 * Timers are real uv_timer_t handles on uv_default_loop() -- the same loop
 * Sxn.serve, Sxn.file and the curl bridge register on -- so the process
 * genuinely sleeps between fires rather than polling. poll() is one
 * UV_RUN_ONCE, which blocks until the next batch of I/O is ready and then
 * returns so the caller can drain the jobs it enqueued. */

#include "sxn_loop.h"

#include <uv.h>
#include <stdlib.h>

/* One handle per timer, heap-allocated because uv_close is asynchronous:
   the handle has to outlive the stop call by however long libuv takes to
   retire it, which is why the runtime's SxnTimer cannot hold it inline any
   more. `fire`/`arg` are the runtime's callback, untouched here. */
typedef struct {
    uv_timer_t handle;
    void (*fire)(void *);
    void *arg;
} SxnUvTimer;

static void sxn_uv_timer_cb(uv_timer_t *h) {
    SxnUvTimer *t = (SxnUvTimer *)h->data;
    t->fire(t->arg);
}

static void sxn_uv_timer_closed(uv_handle_t *h) { free(h->data); }

static void *sxn_uv_timer_start(void *user, uint64_t delay_ms, int repeat,
                                void (*fire)(void *), void *arg) {
    (void)user;
    SxnUvTimer *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fire = fire;
    t->arg = arg;
    uv_timer_init(uv_default_loop(), &t->handle);
    t->handle.data = t;
    uv_timer_start(&t->handle, sxn_uv_timer_cb, delay_ms, repeat ? delay_ms : 0);
    return t;
}

static void sxn_uv_timer_stop(void *user, void *handle) {
    (void)user;
    SxnUvTimer *t = (SxnUvTimer *)handle;
    if (!t) return;
    uv_timer_stop(&t->handle);
    /* uv_close is the only way to retire a handle safely; the free happens in
       its callback, after libuv is done with the memory. */
    uv_close((uv_handle_t *)&t->handle, sxn_uv_timer_closed);
}

static int sxn_uv_poll(void *user, int block, uint64_t max_wait_ms,
                       uint64_t *blocked_ns) {
    (void)user; (void)max_wait_ms;
    uint64_t before = uv_hrtime();
    /* UV_RUN_ONCE blocks until something is ready; UV_RUN_NOWAIT does the
       same sweep without waiting, which is what a host-driven caller wants.
       max_wait_ms is ignored: libuv has no bounded-wait mode, and the one
       caller that would use it (a host frame pump) passes block == 0. */
    int more = uv_run(uv_default_loop(), block ? UV_RUN_ONCE : UV_RUN_NOWAIT);
    *blocked_ns = uv_hrtime() - before;
    return more;
}

static uint64_t sxn_uv_now_ns(void *user) { (void)user; return uv_hrtime(); }

static const SxnLoopOps sxn_uv_ops = {
    .size = sizeof(SxnLoopOps),
    .timer_start = sxn_uv_timer_start,
    .timer_stop = sxn_uv_timer_stop,
    .poll = sxn_uv_poll,
    .now_ns = sxn_uv_now_ns,
};

const SxnLoopOps *sxn_default_loop_ops(void) { return &sxn_uv_ops; }
