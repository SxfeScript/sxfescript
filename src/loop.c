/* Which loop backend is in effect.
 *
 * The selection is a variable rather than a compile-time constant so an
 * embedder can install its own before sxn_install_network. Everything in the
 * runtime reaches the loop through the four calls below; nothing else in
 * src/ names a backend. */

#include "sxn_loop.h"
#include "sxn_clock.h"

#include <stddef.h>

static const SxnLoopOps *sxn_ops;
static void *sxn_ops_user;

void sxn_set_loop_ops(const SxnLoopOps *ops, void *user) {
    sxn_ops = ops;
    sxn_ops_user = user;
}

static const SxnLoopOps *sxn_current_ops(void) {
    return sxn_ops ? sxn_ops : sxn_default_loop_ops();
}

static void *sxn_current_user(void) {
    return sxn_ops ? sxn_ops_user : NULL;
}

void *sxn_loop_timer_start(uint64_t delay_ms, int repeat,
                           void (*fire)(void *), void *arg) {
    return sxn_current_ops()->timer_start(sxn_current_user(), delay_ms, repeat,
                                          fire, arg);
}

void sxn_loop_timer_stop(void *handle) {
    sxn_current_ops()->timer_stop(sxn_current_user(), handle);
}

int sxn_loop_poll(int block, uint64_t max_wait_ms, uint64_t *blocked_ns) {
    uint64_t ignored = 0;
    if (!blocked_ns) blocked_ns = &ignored;
    *blocked_ns = 0;
    return sxn_current_ops()->poll(sxn_current_user(), block, max_wait_ms,
                                   blocked_ns);
}

uint64_t sxn_loop_now_ns(void) {
    const SxnLoopOps *ops = sxn_current_ops();
    /* now_ns is optional: a backend with no better answer than the platform's
       leaves it NULL rather than writing a forwarder. */
    if (ops->now_ns) return ops->now_ns(sxn_current_user());
    return sxn_monotonic_ns();
}
