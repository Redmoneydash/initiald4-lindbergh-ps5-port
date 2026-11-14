/* win_compat.h - GNU-isms -> MSVC, force-included via /FI */
#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H

#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#ifndef __thread
#define __thread __declspec(thread)
#endif

#ifndef __builtin_memcpy
#define __builtin_memcpy memcpy
#endif
#ifndef __builtin_snprintf
#define __builtin_snprintf snprintf
#endif
#ifndef __builtin_va_start
#define __builtin_va_start va_start
#endif
#ifndef __builtin_va_end
#define __builtin_va_end va_end
#endif
#ifndef __builtin_trap
#define __builtin_trap() __debugbreak()
#endif
#ifndef __builtin_return_address
#define __builtin_return_address(x) _ReturnAddress()
#endif
#ifndef __builtin_frame_address
#define __builtin_frame_address(x) NULL
#endif
#ifndef __builtin_expect
#define __builtin_expect(x, v) (x)
#endif

/* GCC attributes are no-ops under MSVC; 'weak' decls just become normal decls.
 * Only used on symbols that are never referenced on this target. */
#ifndef __attribute__
#define __attribute__(x)
#endif

/* clock_gettime / timespec for the few call sites (syscall.c, hle_glut.c).
 * CLOCK_REALTIME is UCRT's timespec_get; CLOCK_MONOTONIC is the QPC counter. */
#include <time.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#define CLOCK_MONOTONIC 2
#endif
static int clock_gettime(int clk, struct timespec *ts)
{
    if (clk == CLOCK_MONOTONIC) {
        /* Wall-clock time can step backwards (NTP, user change); anything
         * computing an elapsed interval needs a clock that cannot. */
        static LARGE_INTEGER freq;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec  = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
        return 0;
    }
    return timespec_get(ts, TIME_UTC) ? 0 : -1;
}

#endif
