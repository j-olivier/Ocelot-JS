/*
 * QuickJS libc host-binding PAL (process spawn/wait/kill)
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
#ifndef QUICKJS_LIBC_PAL_H
#define QUICKJS_LIBC_PAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque per-instance handle, mirroring JSPal (quickjs-pal.h): a custom
   libc PAL defines its own concrete struct and casts pointers to/from
   JSLibcPal *; quickjs-libc.c only stores and forwards the pointer. */
typedef struct JSLibcPal JSLibcPal;

/* options/status helpers for process_wait(), exposed to JS as os.WNOHANG.
   On POSIX this is bit-for-bit the real WNOHANG from <sys/wait.h> and the
   status word is whatever the kernel's waitpid() produced, so the
   WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG macros below are just the
   real ones. Windows has neither concept: the libc PAL's Windows backend
   defines its own WNOHANG bit and always synthesizes a status word with
   the low 7 bits clear (mimicking a POSIX "exited via _exit/return, no
   signal" status), so WIFEXITED is unconditionally true, WEXITSTATUS
   unpacks the exit code, and WIFSIGNALED is always false (there being no
   Windows equivalent of a signaled child). */
#if !defined(_WIN32)
#include <sys/wait.h>
#define JS_LIBC_WNOHANG             WNOHANG
#define JS_LIBC_WIFEXITED(status)   WIFEXITED(status)
#define JS_LIBC_WEXITSTATUS(status) WEXITSTATUS(status)
#define JS_LIBC_WIFSIGNALED(status) WIFSIGNALED(status)
#define JS_LIBC_WTERMSIG(status)    WTERMSIG(status)
#else
#define JS_LIBC_WNOHANG             1
#define JS_LIBC_WIFEXITED(status)   1
#define JS_LIBC_WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define JS_LIBC_WIFSIGNALED(status) 0
#define JS_LIBC_WTERMSIG(status)    0
#endif

typedef struct JSLibcPalFunctions {
    JSLibcPal *opaque;

    /* Spawn a child process. `file` is the executable to run (NULL means
       "use argv[0]"); `argv` is a NULL-terminated argument vector (argv[0]
       is the child's own conventional program name, independent of which
       binary `file`/PATH search actually resolves to); `envp` is a
       NULL-terminated environment (NULL means "inherit the caller's
       environment"); `cwd` may be NULL ("inherit"). `std_fds[3]` gives the
       fds to use as the child's stdin/stdout/stderr. `use_path` selects
       PATH search vs an exact path. `uid`/`gid` of (uint32_t)-1 mean
       "don't change" (POSIX only -- the Windows backend ignores them,
       there being no equivalent). On success returns 0 and stores the new
       process's pid in *out_pid; on failure returns -errno. */
    int (*process_spawn)(JSLibcPal *opaque, const char *file,
                          char *const argv[], char *const envp[],
                          const char *cwd, const int std_fds[3],
                          int use_path, uint32_t uid, uint32_t gid,
                          int *out_pid);

    /* Wait for `pid` to change state (mirrors waitpid()). `options` is 0
       or JS_LIBC_WNOHANG. Returns `pid` and fills *pstatus (decodable via
       JS_LIBC_WIFEXITED & co above) if the process has exited; 0 if
       JS_LIBC_WNOHANG was given and the process is still running; -errno
       on failure. */
    int (*process_wait)(JSLibcPal *opaque, int pid, int options, int *pstatus);

    /* Send signal `sig` to `pid` (mirrors kill()). Returns 0 on success,
       -errno on failure. The Windows backend has no facility to deliver
       arbitrary POSIX signals to another process, so it always terminates
       the process via TerminateProcess() regardless of `sig`. */
    int (*process_kill)(JSLibcPal *opaque, int pid, int sig);
} JSLibcPalFunctions;

/* the default libc PAL, used by quickjs-libc.c */
extern const JSLibcPalFunctions js_libc_pal;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QUICKJS_LIBC_PAL_H */
