/*
 * SDL-shaped names for code ported from green-nx.
 *
 * green-nx's stream engine is Switch/SDL2 code, but its only real use of SDL
 * is the millisecond clock: 34 calls to SDL_GetTicks64 and one performance
 * counter. This device has no SDL at all (see README: SDL's KMSDRM backend
 * drags in the Mali EGL blob), so rather than rewrite every call site -- and
 * make every future rebase against green-nx a manual merge -- we keep the
 * names and back them with CLOCK_MONOTONIC.
 *
 * Include this INSTEAD of <SDL2/SDL.h> in ported files.
 */
#ifndef XCLOUD_NET_COMPAT_H
#define XCLOUD_NET_COMPAT_H

#include <stdint.h>
#include <time.h>

typedef uint64_t Uint64;

#ifdef __cplusplus
extern "C" {
#endif

/* Milliseconds since an arbitrary fixed point. CLOCK_MONOTONIC, so it never
 * jumps when the clock is set -- the watchdogs in engine.cpp compare it
 * across threads and an NTP step would read as a multi-second stall. */
static inline Uint64 SDL_GetTicks64(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (Uint64)ts.tv_sec * 1000u + (Uint64)(ts.tv_nsec / 1000000);
}

static inline Uint64 SDL_GetPerformanceCounter(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

static inline Uint64 SDL_GetPerformanceFrequency(void) { return 1000000000ull; }

#ifdef __cplusplus
}
#endif

#endif /* XCLOUD_NET_COMPAT_H */
