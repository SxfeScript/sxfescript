#ifndef SXN_LOOP_H
#define SXN_LOOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the runtime's waiting happens.
 *
 * Two things in the WinterTC surface need something outside the engine to
 * make progress: a timer has to fire later, and a promise waiting on I/O has
 * to be given a chance to settle. Everything else -- TextEncoder, URL,
 * Headers, the Streams, structuredClone, crypto -- is the engine and nothing
 * else. Those two are what this interface is for, and it is deliberately no
 * larger: no sockets, no filesystem, no descriptor polling. A backend that
 * had to answer those would be a second libuv.
 *
 * sxn ships two implementations. The libuv one is the default and drives
 * uv_timer_t and uv_run exactly as before. The built-in one has no dependency
 * beyond libc and, when fetch is compiled in, libcurl -- which waits on its
 * own sockets, so it can still genuinely sleep rather than spin.
 *
 * The third is the point of the interface: a host that already has a loop --
 * a game engine's frame pump, an application's main loop -- supplies these
 * four functions and calls sxn_runtime_tick once a frame. It never calls
 * sxn_run_event_loop, and the runtime never blocks inside it. */
typedef struct SxnLoopOps {
    /* sizeof(SxnLoopOps) as the caller saw it, so this can gain entries
       without breaking an embedder compiled against an older header. */
    uint32_t size;

    /* Arrange for fire(arg) to be called after delay_ms, and every delay_ms
       after that if repeat. Returns an opaque handle for timer_stop, or NULL
       if the timer could not be created (the caller treats that as a timer
       that never fires rather than an error). */
    void *(*timer_start)(void *user, uint64_t delay_ms, int repeat,
                         void (*fire)(void *), void *arg);

    /* Cancel a handle from timer_start. After this returns, fire must not be
       called again for that handle. The backend owns whatever deferred
       teardown that needs. */
    void (*timer_stop)(void *user, void *handle);

    /* Let the host make progress once.
     *
     * `block` is a permission, not an instruction: the backend may sleep, but
     * for no longer than max_wait_ms (UINT64_MAX for "as long as you like").
     * With block == 0 it must return promptly, having done whatever it can
     * without waiting -- that is the mode a host-driven embed uses.
     *
     * *blocked_ns receives how long it actually waited, which is what the
     * idle cycle sweep reads to decide the process is quiet enough to spend
     * a collection on. A backend that does not wait writes 0, and the sweep
     * then never fires on its own; see sxn_runtime_tick's blocked_hint_ns.
     *
     * Returns nonzero while the host still holds work that could settle a
     * promise -- a pending timer, a transfer in flight, an open listener.
     * Zero means nothing here will ever make progress again, which is how
     * sxn_run_event_loop knows the program is finished. */
    int (*poll)(void *user, int block, uint64_t max_wait_ms,
                uint64_t *blocked_ns);

    /* Monotonic nanoseconds. NULL means "use the runtime's own clock"
       (src/clock.c), which is the same counter libuv reads. A backend that
       already has one supplies it so both agree on what a millisecond is. */
    uint64_t (*now_ns)(void *user);
} SxnLoopOps;

/* Install a backend. Call before sxn_install_network; passing NULL restores
   the one this build was compiled with. `user` is handed back to every
   function above and is never inspected. */
void sxn_set_loop_ops(const SxnLoopOps *ops, void *user);

/* --- internal to the runtime -------------------------------------------
   How src/ reaches the installed backend. An embedder implements SxnLoopOps
   above and never calls these; they are here rather than in a header of
   their own because there is one loop and this is its file. */
void    *sxn_loop_timer_start(uint64_t delay_ms, int repeat,
                              void (*fire)(void *), void *arg);
void     sxn_loop_timer_stop(void *handle);
int      sxn_loop_poll(int block, uint64_t max_wait_ms, uint64_t *blocked_ns);
uint64_t sxn_loop_now_ns(void);
/* The backend compiled into this build: src/loop_uv.c or src/loop_builtin.c. */
const SxnLoopOps *sxn_default_loop_ops(void);

#ifdef __cplusplus
}
#endif
#endif
