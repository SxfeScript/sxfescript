#ifndef SXN_CLOCK_H
#define SXN_CLOCK_H

#include <stdint.h>

/* Nanoseconds from an unspecified origin, never going backwards. The origin
   is whatever the platform counts from, so only differences mean anything.
   src/clock.c has which counter each platform uses and why. */
uint64_t sxn_monotonic_ns(void);

#endif
