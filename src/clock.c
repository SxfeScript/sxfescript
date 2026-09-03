/* The monotonic clock the runtime measures with.
 *
 * performance.now() is bound straight to this (see bootstrap.js), and the
 * idle-sweep gate in src/network.c times how long the loop blocked with it,
 * so it is on a path that is measured rather than merely used --
 * spec/PERFORMANCE.md quotes the cost of the read itself.
 *
 * It was uv_hrtime(), which meant the WinterTC half could not be built
 * without libuv for the sake of a clock. This is the same clock libuv itself
 * reads: clock_gettime(CLOCK_MONOTONIC) on Linux and the BSDs,
 * mach_absolute_time on Darwin, QueryPerformanceCounter on Windows. A loop
 * backend that has a better answer supplies one through SxnLoopOps.now_ns;
 * the libuv backend passes uv_hrtime so the default build reads exactly what
 * it read before. */

#include "sxn_clock.h"

#if defined(_WIN32)
#include <windows.h>

uint64_t sxn_monotonic_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    /* Split the division so a counter near 2^63 cannot overflow the multiply,
       which it would after a few hours at a 10 MHz frequency. */
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000000ull +
           (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000000ull / (uint64_t)freq.QuadPart;
}

#elif defined(__APPLE__)
#include <mach/mach_time.h>

uint64_t sxn_monotonic_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    /* On every Apple platform shipped so far numer == denom == 1, so this is
       the counter itself; the multiply is here for the day that changes. */
    if (tb.numer == tb.denom) return mach_absolute_time();
    return mach_absolute_time() * tb.numer / tb.denom;
}

#else
#include <time.h>

uint64_t sxn_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
