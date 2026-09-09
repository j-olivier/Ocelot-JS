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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#endif

#include "quickjs-pal.h"

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
/* time */

static void pal_get_time(JSPal *opaque, JSPalTime *t)
{
    struct timeval tv;
    (void)opaque;
    gettimeofday(&tv, NULL);
    t->sec = tv.tv_sec;
    t->usec = tv.tv_usec;
}

static void pal_get_time_monotonic(JSPal *opaque, JSPalTime *t)
{
    struct timespec ts;
    (void)opaque;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->sec = ts.tv_sec;
    t->usec = ts.tv_nsec / 1000;
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

static_assert(sizeof(pthread_mutex_t) <= sizeof(JSPalMutex),
              "pthread_mutex_t too big for JSPalMutex");
static_assert(sizeof(pthread_cond_t) <= sizeof(JSPalCond),
              "pthread_cond_t too big for JSPalCond");
static_assert(sizeof(pthread_t) <= sizeof(JSPalThread),
              "pthread_t too big for JSPalThread");

static void pal_mutex_init(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
    pthread_mutex_init((pthread_mutex_t *)mutex, NULL);
}

static void pal_mutex_destroy(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
}

static void pal_mutex_lock(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
    pthread_mutex_lock((pthread_mutex_t *)mutex);
}

static void pal_mutex_unlock(JSPal *opaque, JSPalMutex *mutex)
{
    (void)opaque;
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

static void pal_cond_init(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
    pthread_cond_init((pthread_cond_t *)cond, NULL);
}

static void pal_cond_destroy(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
    pthread_cond_destroy((pthread_cond_t *)cond);
}

static void pal_cond_wait(JSPal *opaque, JSPalCond *cond, JSPalMutex *mutex)
{
    (void)opaque;
    pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
}

static int pal_cond_timedwait(JSPal *opaque, JSPalCond *cond, JSPalMutex *mutex, const JSPalTime *abstime)
{
    struct timespec ts;
    (void)opaque;
    ts.tv_sec = abstime->sec;
    ts.tv_nsec = abstime->usec * 1000;
    return pthread_cond_timedwait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex, &ts);
}

static void pal_cond_signal(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
    pthread_cond_signal((pthread_cond_t *)cond);
}

static void pal_cond_broadcast(JSPal *opaque, JSPalCond *cond)
{
    (void)opaque;
    pthread_cond_broadcast((pthread_cond_t *)cond);
}

/*----------------------------------------------------------------------*/
/* threads */

static int pal_thread_create(JSPal *opaque, JSPalThread *thread, void *(*start)(void *arg), void *arg,
                              size_t stack_size)
{
    pthread_attr_t attr;
    int ret;

    (void)opaque;
    pthread_attr_init(&attr);
    if (stack_size != 0)
        pthread_attr_setstacksize(&attr, stack_size);
    ret = pthread_create((pthread_t *)thread, &attr, start, arg);
    pthread_attr_destroy(&attr);
    return ret;
}

static int pal_thread_join(JSPal *opaque, JSPalThread *thread)
{
    (void)opaque;
    return pthread_join(*(pthread_t *)thread, NULL);
}

/*----------------------------------------------------------------------*/

const JSPalFunctions js_pal = {
    .abort = pal_abort,
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
