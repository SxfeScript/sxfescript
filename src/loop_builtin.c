/* The loop backend with no libuv in it.
 *
 * Enough to keep the WinterTC surface whole where there is no libuv to link:
 * timers, so setTimeout and setInterval fire, and a poll that lets libcurl
 * wait on its own sockets so fetch still streams and the process still
 * sleeps rather than spins.
 *
 * What it does not have is Sxn.serve and Sxn.file's async reads, which are a
 * TCP listener and a thread pool and have no portable stand-in worth
 * writing. Those are capabilities of the libuv backend and are compiled out
 * here; neither is a WinterTC name, so the surface stays complete.
 *
 * Timers are a delay-sorted singly-linked list. A list is the right shape at
 * this size: a program with enough concurrent timers for a heap to pay is a
 * program that wants the libuv backend anyway. */

#include "sxn_loop.h"
#include "sxn_clock.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
static void sxn_sleep_ms(uint64_t ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sxn_sleep_ms(uint64_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000ull);
    nanosleep(&ts, NULL);
}
#endif

typedef struct SxnBuiltinTimer {
    uint64_t due_ns;       /* when it should next fire */
    uint64_t period_ns;    /* 0 for a one-shot */
    void (*fire)(void *);
    void *arg;
    int dead;              /* stopped while the list was being walked */
    struct SxnBuiltinTimer *next;
} SxnBuiltinTimer;

static SxnBuiltinTimer *sxn_bt_head;   /* sorted by due_ns, soonest first */
static int sxn_bt_walking;             /* inside sxn_builtin_poll's walk */

static void sxn_bt_insert(SxnBuiltinTimer *t) {
    SxnBuiltinTimer **slot = &sxn_bt_head;
    while (*slot && (*slot)->due_ns <= t->due_ns) slot = &(*slot)->next;
    t->next = *slot;
    *slot = t;
}

static void sxn_bt_unlink(SxnBuiltinTimer *t) {
    for (SxnBuiltinTimer **slot = &sxn_bt_head; *slot; slot = &(*slot)->next) {
        if (*slot == t) { *slot = t->next; return; }
    }
}

/* Drop everything cancelled during a walk. Doing it here rather than in
   timer_stop is what lets a timer cancel itself, or another one, from inside
   its own callback without the walk losing its place. */
static void sxn_bt_reap(void) {
    SxnBuiltinTimer **slot = &sxn_bt_head;
    while (*slot) {
        SxnBuiltinTimer *t = *slot;
        if (t->dead) { *slot = t->next; free(t); }
        else slot = &t->next;
    }
}

static void *sxn_builtin_timer_start(void *user, uint64_t delay_ms, int repeat,
                                     void (*fire)(void *), void *arg) {
    (void)user;
    SxnBuiltinTimer *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    uint64_t period = delay_ms * 1000000ull;
    t->due_ns = sxn_monotonic_ns() + period;
    t->period_ns = repeat ? period : 0;
    t->fire = fire;
    t->arg = arg;
    sxn_bt_insert(t);
    return t;
}

static void sxn_builtin_timer_stop(void *user, void *handle) {
    (void)user;
    SxnBuiltinTimer *t = (SxnBuiltinTimer *)handle;
    if (!t || t->dead) return;
    t->dead = 1;
    if (sxn_bt_walking) return;   /* sxn_bt_reap will take it */
    sxn_bt_unlink(t);
    free(t);
}

/* Nonzero while anything could still fire. */
static int sxn_builtin_has_work(void) {
    for (SxnBuiltinTimer *t = sxn_bt_head; t; t = t->next)
        if (!t->dead) return 1;
    return 0;
}

/* How long until the soonest timer, or UINT64_MAX if there is none. */
static uint64_t sxn_builtin_next_wait_ms(uint64_t now) {
    for (SxnBuiltinTimer *t = sxn_bt_head; t; t = t->next) {
        if (t->dead) continue;
        return t->due_ns <= now ? 0 : (t->due_ns - now) / 1000000ull;
    }
    return UINT64_MAX;
}

static int sxn_builtin_poll(void *user, int block, uint64_t max_wait_ms,
                            uint64_t *blocked_ns) {
    (void)user;
    uint64_t started = sxn_monotonic_ns();

    /* Fire everything due. The list is re-read each time round because a
       callback may add, cancel or re-arm timers, including its own. */
    sxn_bt_walking = 1;
    for (;;) {
        uint64_t now = sxn_monotonic_ns();
        SxnBuiltinTimer *t = sxn_bt_head;
        while (t && (t->dead || t->due_ns > now)) t = t->dead ? t->next : NULL;
        if (!t) break;
        sxn_bt_unlink(t);
        if (t->period_ns) {
            /* Re-arm from now, not from the deadline: a callback that runs
               longer than the interval must not build up a backlog it can
               never work off. */
            t->due_ns = now + t->period_ns;
            sxn_bt_insert(t);
        }
        t->fire(t->arg);
        if (!t->period_ns && !t->dead) free(t);   /* one-shot, still ours */
    }
    sxn_bt_walking = 0;
    sxn_bt_reap();

    /* Let any transfer in flight make progress, and let libcurl do the
       waiting if it has sockets to wait on -- it is the only thing here with
       a descriptor. Returns nonzero while a transfer is running. */
    uint64_t now = sxn_monotonic_ns();
    uint64_t wait_ms = sxn_builtin_next_wait_ms(now);
    if (wait_ms > max_wait_ms) wait_ms = max_wait_ms;
    int transfers = sxn_curl_pump(block, wait_ms);

    if (block && !transfers && wait_ms != UINT64_MAX && wait_ms > 0) {
        /* Nothing but timers left, and libcurl did not wait for us. */
        sxn_sleep_ms(wait_ms);
    }
    *blocked_ns = sxn_monotonic_ns() - started;
    return sxn_builtin_has_work() || transfers;
}

static const SxnLoopOps sxn_builtin_ops = {
    .size = sizeof(SxnLoopOps),
    .timer_start = sxn_builtin_timer_start,
    .timer_stop = sxn_builtin_timer_stop,
    .poll = sxn_builtin_poll,
    .now_ns = NULL,               /* src/clock.c is already the right answer */
};

const SxnLoopOps *sxn_default_loop_ops(void) { return &sxn_builtin_ops; }
