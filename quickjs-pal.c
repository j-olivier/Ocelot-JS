/*
 * QuickJS default PAL (Platform Abstraction Layer) implementation
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* All CI targets for this project (Linux, macOS, *BSD, Cosmopolitan, and
   Windows via MSYS2/MinGW) provide a real pthreads implementation --
   quickjs.c already assumed this unconditionally (CONFIG_ATOMICS pulls in
   <pthread.h> everywhere except __EMSCRIPTEN__) before this PAL existed.
   So the default PAL's synchronization primitives are pthreads-based on
   every platform; only the timezone-offset computation genuinely differs
   between native MSVC-style Windows and everything else, and that split
   already existed in quickjs.c prior to this refactor -- it is carried
   over here unchanged. */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* must come before the QJS_MSVC checks below -- that macro is defined by
   quickjs.h, pulled in transitively through this header */
#include "quickjs-pal.h"

#if !QJS_MSVC
/* MinGW and every Unix target: real pthreads (winpthreads on MinGW). */
#include <sys/time.h>
#include <pthread.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#elif defined(_WIN32)
#include <malloc.h>
#endif

/*----------------------------------------------------------------------*/
/* panic */

void js_abort(void)
{
    abort();
}

static void pal_abort(JSPal *opaque) __attribute__((noreturn));

static void pal_abort(JSPal *opaque)
{
    (void)opaque;
    js_abort();
}

/*----------------------------------------------------------------------*/
/* debug output */

static int pal_printf(JSPal *opaque, const char *format, ...)
{
    va_list ap;
    int ret;

    (void)opaque;
    va_start(ap, format);
    ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}

/*----------------------------------------------------------------------*/
/* time */

static void pal_get_time(JSPal *opaque, JSPalTime *t)
{
    (void)opaque;
#if QJS_MSVC
    FILETIME ft;
    ULARGE_INTEGER uli;
    uint64_t epoch_us;

    /* FILETIME is 100ns ticks since 1601-01-01; 11644473600 is the number of
       seconds between that epoch and the Unix epoch (1970-01-01). */
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    epoch_us = uli.QuadPart / 10 - UINT64_C(11644473600000000);
    t->sec = (int64_t)(epoch_us / 1000000);
    t->usec = (int64_t)(epoch_us % 1000000);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    t->sec = tv.tv_sec;
    t->usec = tv.tv_usec;
#endif
}

static void pal_get_time_monotonic(JSPal *opaque, JSPalTime *t)
{
    (void)opaque;
#if QJS_MSVC
    LARGE_INTEGER freq, counter;
    double seconds;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    seconds = (double)counter.QuadPart / (double)freq.QuadPart;
    t->sec = (int64_t)seconds;
    t->usec = (int64_t)((seconds - (double)t->sec) * 1e6);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->sec = ts.tv_sec;
    t->usec = ts.tv_nsec / 1000;
#endif
}

/* returns the offset in minutes from UTC to local time for the given
   time (ms since epoch), matching JS Date.prototype.getTimezoneOffset()
   sign conventions. This is a verbatim relocation of the pre-PAL
   getTimezoneOffset() that used to live in quickjs.c. */
static int pal_get_timezone_offset(JSPal *opaque, int64_t time)
{
    (void)opaque;
    time_t ti;
    int res;

    time /= 1000; /* convert to seconds */
    if (sizeof(time_t) == 4) {
        /* on 32-bit systems, we need to clamp the time value to the
           range of `time_t`. This is better than truncating values to
           32 bits and hopefully provides the same result as 64-bit
           implementation of localtime_r.
         */
        if ((time_t)-1 < 0) {
            if (time < INT32_MIN) {
                time = INT32_MIN;
            } else if (time > INT32_MAX) {
                time = INT32_MAX;
            }
        } else {
            if (time < 0) {
                time = 0;
            } else if (time > UINT32_MAX) {
                time = UINT32_MAX;
            }
        }
    }
    ti = time;
#if defined(_WIN32)
    {
        struct tm *tm;
        time_t gm_ti, loc_ti;

        tm = gmtime(&ti);
        if (!tm)
            return 0;
        gm_ti = mktime(tm);

        tm = localtime(&ti);
        if (!tm)
            return 0;
        loc_ti = mktime(tm);

        res = (gm_ti - loc_ti) / 60;
    }
#else
    {
        struct tm tm;
        localtime_r(&ti, &tm);
        res = -tm.tm_gmtoff / 60;
    }
#endif
    return res;
}

/*----------------------------------------------------------------------*/
/* memory allocator, backing the default JSMallocFunctions impl only (see
   the comment above JSPalTime in quickjs.h) */

static void *pal_memory_malloc(JSPal *opaque, size_t size)
{
    (void)opaque;
    return malloc(size);
}

static void pal_memory_free(JSPal *opaque, void *ptr)
{
    (void)opaque;
    free(ptr);
}

static void *pal_memory_realloc(JSPal *opaque, void *ptr, size_t size)
{
    (void)opaque;
    return realloc(ptr, size);
}

static size_t pal_memory_malloc_usable_size(JSPal *opaque, const void *ptr)
{
    (void)opaque;
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize((void *)ptr);
#elif defined(__EMSCRIPTEN__)
    return 0;
#elif defined(__linux__) || defined(__GLIBC__)
    return malloc_usable_size((void *)ptr);
#else
    /* change this to `return 0;` if compilation fails */
    return malloc_usable_size((void *)ptr);
#endif
}

/*----------------------------------------------------------------------*/
/* mutex / condition variables */

#if QJS_MSVC
static_assert(sizeof(SRWLOCK) <= sizeof(JSPalMutex),
              "SRWLOCK too big for JSPalMutex");
static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(JSPalCond),
              "CONDITION_VARIABLE too big for JSPalCond");
static_assert(sizeof(HANDLE) <= sizeof(JSPalThread),
              "HANDLE too big for JSPalThread");
#else
static_assert(sizeof(pthread_mutex_t) <= sizeof(JSPalMutex),
              "pthread_mutex_t too big for JSPalMutex");
static_assert(sizeof(pthread_cond_t) <= sizeof(JSPalCond),
              "pthread_cond_t too big for JSPalCond");
static_assert(sizeof(pthread_t) <= sizeof(JSPalThread),
              "pthread_t too big for JSPalThread");
#endif

static void pal_mutex_init(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
#if QJS_MSVC
    InitializeSRWLock((PSRWLOCK)mutex);
#else
    pthread_mutex_init((pthread_mutex_t *)mutex, NULL);
#endif
}

static void pal_mutex_destroy(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
#if QJS_MSVC
    /* SRWLOCK requires no destruction. */
    (void)mutex;
#else
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
#endif
}

static void pal_mutex_lock(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
#if QJS_MSVC
    AcquireSRWLockExclusive((PSRWLOCK)mutex);
#else
    pthread_mutex_lock((pthread_mutex_t *)mutex);
#endif
}

static void pal_mutex_unlock(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
#if QJS_MSVC
    ReleaseSRWLockExclusive((PSRWLOCK)mutex);
#else
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
#endif
}

static void pal_cond_init(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
#if QJS_MSVC
    InitializeConditionVariable((PCONDITION_VARIABLE)cond);
#else
    pthread_cond_init((pthread_cond_t *)cond, NULL);
#endif
}

static void pal_cond_destroy(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
#if QJS_MSVC
    /* CONDITION_VARIABLE requires no destruction. */
    (void)cond;
#else
    pthread_cond_destroy((pthread_cond_t *)cond);
#endif
}

static void pal_cond_wait(JSPal *opaque, JSPalCond *cond, JSPalMutex *mutex)
{
    (void)opaque;
#if QJS_MSVC
    SleepConditionVariableSRW((PCONDITION_VARIABLE)cond, (PSRWLOCK)mutex, INFINITE, 0);
#else
    pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
#endif
}

static int pal_cond_timedwait(JSPal *opaque, JSPalCond *cond, JSPalMutex *mutex, const JSPalTime *abstime)
{
#if QJS_MSVC
    JSPalTime now;
    int64_t delta_ms;
    DWORD timeout_ms;

    pal_get_time(opaque, &now);
    delta_ms = (abstime->sec - now.sec) * 1000 + (abstime->usec - now.usec) / 1000;
    timeout_ms = delta_ms > 0 ? (DWORD)delta_ms : 0;
    if (SleepConditionVariableSRW((PCONDITION_VARIABLE)cond, (PSRWLOCK)mutex, timeout_ms, 0))
        return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : -1;
#else
    struct timespec ts;
    (void)opaque;
    ts.tv_sec = abstime->sec;
    ts.tv_nsec = abstime->usec * 1000;
    return pthread_cond_timedwait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex, &ts);
#endif
}

static void pal_cond_signal(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
#if QJS_MSVC
    WakeConditionVariable((PCONDITION_VARIABLE)cond);
#else
    pthread_cond_signal((pthread_cond_t *)cond);
#endif
}

static void pal_cond_broadcast(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
#if QJS_MSVC
    WakeAllConditionVariable((PCONDITION_VARIABLE)cond);
#else
    pthread_cond_broadcast((pthread_cond_t *)cond);
#endif
}

/*----------------------------------------------------------------------*/
/* threads */

#if QJS_MSVC
/* CreateThread wants a DWORD WINAPI(LPVOID) entry point; adapt the
   void *(*)(void *arg) start routine via a heap-allocated trampoline. */
typedef struct PalThreadTrampolineArgs {
    void *(*start)(void *arg);
    void *arg;
} PalThreadTrampolineArgs;

static DWORD WINAPI pal_thread_trampoline(LPVOID param)
{
    PalThreadTrampolineArgs *targs = (PalThreadTrampolineArgs *)param;
    void *(*start)(void *arg) = targs->start;
    void *arg = targs->arg;

    free(targs);
    start(arg);
    return 0;
}
#endif

static int pal_thread_create(JSPal *opaque, JSPalThread *thread, void *(*start)(void *arg), void *arg,
                              size_t stack_size)
{
    (void)opaque;
#if QJS_MSVC
    PalThreadTrampolineArgs *targs;
    HANDLE h;

    targs = malloc(sizeof(*targs));
    if (!targs)
        return -1;
    targs->start = start;
    targs->arg = arg;
    h = CreateThread(NULL, stack_size, pal_thread_trampoline, targs, 0, NULL);
    if (!h) {
        free(targs);
        return -1;
    }
    *(HANDLE *)thread = h;
    return 0;
#else
    pthread_attr_t attr;
    int ret;

    pthread_attr_init(&attr);
    if (stack_size != 0)
        pthread_attr_setstacksize(&attr, stack_size);
    ret = pthread_create((pthread_t *)thread, &attr, start, arg);
    pthread_attr_destroy(&attr);
    return ret;
#endif
}

static int pal_thread_join(JSPal *opaque, JSPalThread *thread)
{
    (void)opaque;
#if QJS_MSVC
    HANDLE h = *(HANDLE *)thread;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return 0;
#else
    return pthread_join(*(pthread_t *)thread, NULL);
#endif
}

static int pal_thread_detach(JSPal *opaque, JSPalThread *thread)
{
    (void)opaque;
#if QJS_MSVC
    /* CreateThread's HANDLE has no separate "detached" state; just drop
       our reference to it so the thread object is freed once it exits. */
    CloseHandle(*(HANDLE *)thread);
    return 0;
#else
    return pthread_detach(*(pthread_t *)thread);
#endif
}

/*----------------------------------------------------------------------*/

JSPalFunctions js_pal = {
    .abort = pal_abort,
    .print_f = pal_printf,
    .get_time = pal_get_time,
    .get_time_monotonic = pal_get_time_monotonic,
    .get_timezone_offset = pal_get_timezone_offset,
    .memory_malloc = pal_memory_malloc,
    .memory_free = pal_memory_free,
    .memory_realloc = pal_memory_realloc,
    .memory_malloc_usable_size = pal_memory_malloc_usable_size,
    .mutex_init = pal_mutex_init,
    .mutex_destroy = pal_mutex_destroy,
    .mutex_lock = pal_mutex_lock,
    .mutex_unlock = pal_mutex_unlock,
    .cond_init = pal_cond_init,
    .cond_destroy = pal_cond_destroy,
    .cond_wait = pal_cond_wait,
    .cond_timedwait = pal_cond_timedwait,
    .cond_signal = pal_cond_signal,
    .cond_broadcast = pal_cond_broadcast,
    .thread_create = pal_thread_create,
    .thread_join = pal_thread_join,
    .thread_detach = pal_thread_detach,
};

/*----------------------------------------------------------------------*/
/* atomics: plain C11 stdatomic.h wrappers, relocated out of quickjs.c.
   Not part of JSPal -- see quickjs-pal.h for the rationale. */

#define PAL_ATOMIC_OPS(width, uintN_t)                                            \
    uintN_t pal_atomic##width##_load(uintN_t *ptr)                                \
    {                                                                             \
        return atomic_load((_Atomic(uintN_t) *)ptr);                             \
    }                                                                             \
    void pal_atomic##width##_store(uintN_t *ptr, uintN_t v)                       \
    {                                                                             \
        atomic_store((_Atomic(uintN_t) *)ptr, v);                                \
    }                                                                             \
    uintN_t pal_atomic##width##_exchange(uintN_t *ptr, uintN_t v)                 \
    {                                                                             \
        return atomic_exchange((_Atomic(uintN_t) *)ptr, v);                      \
    }                                                                             \
    JS_BOOL pal_atomic##width##_compare_exchange(uintN_t *ptr, uintN_t *expected, \
                                                  uintN_t desired)                \
    {                                                                             \
        return atomic_compare_exchange_strong((_Atomic(uintN_t) *)ptr, expected, \
                                               desired);                          \
    }                                                                             \
    uintN_t pal_atomic##width##_fetch_add(uintN_t *ptr, uintN_t v)                \
    {                                                                             \
        return atomic_fetch_add((_Atomic(uintN_t) *)ptr, v);                     \
    }                                                                             \
    uintN_t pal_atomic##width##_fetch_sub(uintN_t *ptr, uintN_t v)                \
    {                                                                             \
        return atomic_fetch_sub((_Atomic(uintN_t) *)ptr, v);                     \
    }                                                                             \
    uintN_t pal_atomic##width##_fetch_and(uintN_t *ptr, uintN_t v)                \
    {                                                                             \
        return atomic_fetch_and((_Atomic(uintN_t) *)ptr, v);                     \
    }                                                                             \
    uintN_t pal_atomic##width##_fetch_or(uintN_t *ptr, uintN_t v)                 \
    {                                                                             \
        return atomic_fetch_or((_Atomic(uintN_t) *)ptr, v);                      \
    }                                                                             \
    uintN_t pal_atomic##width##_fetch_xor(uintN_t *ptr, uintN_t v)                \
    {                                                                             \
        return atomic_fetch_xor((_Atomic(uintN_t) *)ptr, v);                     \
    }

PAL_ATOMIC_OPS(8, uint8_t)
PAL_ATOMIC_OPS(16, uint16_t)
PAL_ATOMIC_OPS(32, uint32_t)
PAL_ATOMIC_OPS(64, uint64_t)

#undef PAL_ATOMIC_OPS
