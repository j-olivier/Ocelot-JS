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
#ifndef QUICKJS_PAL_H
#define QUICKJS_PAL_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the default PAL, used when JS_NewRuntime()/JS_NewRuntime2() or
   JS_NewRuntimePal(mf, NULL, opaque) are called */
extern const JSPalFunctions js_pal;

/* process-wide panic hook (no per-runtime state involved). noreturn even
   though the JSPal hook it calls is a plain function pointer -- without
   this, GCC/Clang lose the "control flow ends here" information that
   abort() (a builtin) gives them for free, and -Wmaybe-uninitialized
   misfires at every call site that relies on it to make a switch/if
   exhaustive. */
void js_abort(void) __attribute__((noreturn));

/* atomic ops backing the JS Atomics object / SharedArrayBuffer. These are
   compiler/CPU primitives (C11 <stdatomic.h>), not part of JSPal, since
   every supported target has stdatomic.h and there is no realistic host
   that needs to override them. */
uint8_t  pal_atomic8_load(uint8_t *ptr);
uint16_t pal_atomic16_load(uint16_t *ptr);
uint32_t pal_atomic32_load(uint32_t *ptr);
uint64_t pal_atomic64_load(uint64_t *ptr);

void pal_atomic8_store(uint8_t *ptr, uint8_t v);
void pal_atomic16_store(uint16_t *ptr, uint16_t v);
void pal_atomic32_store(uint32_t *ptr, uint32_t v);
void pal_atomic64_store(uint64_t *ptr, uint64_t v);

uint8_t  pal_atomic8_exchange(uint8_t *ptr, uint8_t v);
uint16_t pal_atomic16_exchange(uint16_t *ptr, uint16_t v);
uint32_t pal_atomic32_exchange(uint32_t *ptr, uint32_t v);
uint64_t pal_atomic64_exchange(uint64_t *ptr, uint64_t v);

JS_BOOL pal_atomic8_compare_exchange(uint8_t *ptr, uint8_t *expected, uint8_t desired);
JS_BOOL pal_atomic16_compare_exchange(uint16_t *ptr, uint16_t *expected, uint16_t desired);
JS_BOOL pal_atomic32_compare_exchange(uint32_t *ptr, uint32_t *expected, uint32_t desired);
JS_BOOL pal_atomic64_compare_exchange(uint64_t *ptr, uint64_t *expected, uint64_t desired);

uint8_t  pal_atomic8_fetch_add(uint8_t *ptr, uint8_t v);
uint16_t pal_atomic16_fetch_add(uint16_t *ptr, uint16_t v);
uint32_t pal_atomic32_fetch_add(uint32_t *ptr, uint32_t v);
uint64_t pal_atomic64_fetch_add(uint64_t *ptr, uint64_t v);

uint8_t  pal_atomic8_fetch_sub(uint8_t *ptr, uint8_t v);
uint16_t pal_atomic16_fetch_sub(uint16_t *ptr, uint16_t v);
uint32_t pal_atomic32_fetch_sub(uint32_t *ptr, uint32_t v);
uint64_t pal_atomic64_fetch_sub(uint64_t *ptr, uint64_t v);

uint8_t  pal_atomic8_fetch_and(uint8_t *ptr, uint8_t v);
uint16_t pal_atomic16_fetch_and(uint16_t *ptr, uint16_t v);
uint32_t pal_atomic32_fetch_and(uint32_t *ptr, uint32_t v);
uint64_t pal_atomic64_fetch_and(uint64_t *ptr, uint64_t v);

uint8_t  pal_atomic8_fetch_or(uint8_t *ptr, uint8_t v);
uint16_t pal_atomic16_fetch_or(uint16_t *ptr, uint16_t v);
uint32_t pal_atomic32_fetch_or(uint32_t *ptr, uint32_t v);
uint64_t pal_atomic64_fetch_or(uint64_t *ptr, uint64_t v);

uint8_t  pal_atomic8_fetch_xor(uint8_t *ptr, uint8_t v);
uint16_t pal_atomic16_fetch_xor(uint16_t *ptr, uint16_t v);
uint32_t pal_atomic32_fetch_xor(uint32_t *ptr, uint32_t v);
uint64_t pal_atomic64_fetch_xor(uint64_t *ptr, uint64_t v);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QUICKJS_PAL_H */
