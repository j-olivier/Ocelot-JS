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
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "quickjs-libc-pal.h"

#if defined(_WIN32)

#ifndef _WIN32_WINNT
/* SRWLOCK (used below for the pid->HANDLE table lock) requires Vista+;
   pin this translation unit to it regardless of the toolchain's default. */
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <io.h>

/*----------------------------------------------------------------------*/
/* pid -> HANDLE registry.
   CreateProcess() hands back a HANDLE, but the JS-visible os.exec()/
   waitpid()/kill() API only deals in plain pids (matching the POSIX
   API), so a later waitpid()/kill() call needs to look the HANDLE back
   up from the pid alone. Entries are removed once the process has been
   waited for; a pid not found here (never spawned by us, or already
   reaped) falls back to OpenProcess() by pid, best-effort. */

typedef struct ProcEntry {
    DWORD pid;
    HANDLE handle;
    struct ProcEntry *next;
} ProcEntry;

static SRWLOCK proc_table_lock = SRWLOCK_INIT;
static ProcEntry *proc_table;

static void proc_table_add(DWORD pid, HANDLE handle)
{
    ProcEntry *e = malloc(sizeof(*e));
    if (!e) {
        /* can't track it -- better to leak the handle than to leave the
           child process un-collectible via CloseHandle() right away */
        return;
    }
    e->pid = pid;
    e->handle = handle;
    AcquireSRWLockExclusive(&proc_table_lock);
    e->next = proc_table;
    proc_table = e;
    ReleaseSRWLockExclusive(&proc_table_lock);
}

/* non-owning lookup: does not remove the entry or duplicate the handle */
static HANDLE proc_table_find(DWORD pid)
{
    ProcEntry *e;
    HANDLE h = NULL;

    AcquireSRWLockExclusive(&proc_table_lock);
    for (e = proc_table; e; e = e->next) {
        if (e->pid == pid) {
            h = e->handle;
            break;
        }
    }
    ReleaseSRWLockExclusive(&proc_table_lock);
    return h;
}

static void proc_table_remove(DWORD pid)
{
    ProcEntry **pp, *e;

    AcquireSRWLockExclusive(&proc_table_lock);
    for (pp = &proc_table; (e = *pp) != NULL; pp = &e->next) {
        if (e->pid == pid) {
            *pp = e->next;
            free(e);
            break;
        }
    }
    ReleaseSRWLockExclusive(&proc_table_lock);
}

/*----------------------------------------------------------------------*/
/* command line / environment block construction */

/* Appends `arg`, quoted per the MSVCRT command-line parsing rules, to
   `db`. See
   https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments */
static void win32_append_quoted_arg(DynBuf *db, const char *arg)
{
    int need_quotes;
    const char *p;
    size_t backslashes;

    need_quotes = (*arg == '\0') || (strpbrk(arg, " \t\"") != NULL);
    if (!need_quotes) {
        dbuf_putstr(db, arg);
        return;
    }
    dbuf_putc(db, '"');
    for (p = arg; *p;) {
        backslashes = 0;
        while (*p == '\\') {
            backslashes++;
            p++;
        }
        if (*p == '"') {
            /* backslashes immediately before a quote must be doubled,
               and the quote itself backslash-escaped */
            while (backslashes-- > 0) {
                dbuf_putc(db, '\\');
                dbuf_putc(db, '\\');
            }
            dbuf_putc(db, '\\');
            dbuf_putc(db, '"');
            p++;
        } else if (*p == '\0') {
            /* backslashes right before our own closing quote must also
               be doubled, so they aren't parsed as escaping it */
            while (backslashes-- > 0) {
                dbuf_putc(db, '\\');
                dbuf_putc(db, '\\');
            }
            break;
        } else {
            while (backslashes-- > 0)
                dbuf_putc(db, '\\');
            dbuf_putc(db, *p);
            p++;
        }
    }
    dbuf_putc(db, '"');
}

static char *win32_build_cmdline(char *const argv[])
{
    DynBuf db;
    int i;

    dbuf_init(&db);
    for (i = 0; argv[i]; i++) {
        if (i > 0)
            dbuf_putc(&db, ' ');
        win32_append_quoted_arg(&db, argv[i]);
    }
    dbuf_putc(&db, '\0');
    if (dbuf_error(&db)) {
        dbuf_free(&db);
        return NULL;
    }
    return (char *)db.buf;
}

static char *win32_build_envblock(char *const envp[])
{
    DynBuf db;
    int i;

    if (!envp)
        return NULL; /* NULL to CreateProcessA means "inherit" */
    dbuf_init(&db);
    for (i = 0; envp[i]; i++) {
        dbuf_putstr(&db, envp[i]);
        dbuf_putc(&db, '\0');
    }
    dbuf_putc(&db, '\0'); /* extra NUL: double-NUL terminates the block */
    if (dbuf_error(&db)) {
        dbuf_free(&db);
        return NULL;
    }
    return (char *)db.buf;
}

/*----------------------------------------------------------------------*/

static int libc_process_spawn(JSLibcPal *opaque, const char *file,
                               char *const argv[], char *const envp[],
                               const char *cwd, const int std_fds[3],
                               int use_path, uint32_t uid, uint32_t gid,
                               int *out_pid)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *cmdline, *envblock = NULL;
    const char *app;
    BOOL ok;

    (void)opaque;
    (void)uid; /* no Windows equivalent */
    (void)gid; /* no Windows equivalent */

    cmdline = win32_build_cmdline(argv);
    if (!cmdline)
        return -ENOMEM;
    if (envp) {
        envblock = win32_build_envblock(envp);
        if (!envblock) {
            free(cmdline);
            return -ENOMEM;
        }
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (HANDLE)_get_osfhandle(std_fds[0]);
    si.hStdOutput = (HANDLE)_get_osfhandle(std_fds[1]);
    si.hStdError = (HANDLE)_get_osfhandle(std_fds[2]);

    /* if use_path, let CreateProcess's own search algorithm (which
       includes PATH) resolve argv[0]/file; otherwise force the exact
       path via lpApplicationName, matching execve()'s "no search"
       semantics */
    app = file ? file : argv[0];
    ok = CreateProcessA(use_path ? NULL : app, cmdline, NULL, NULL,
                         /* bInheritHandles */ TRUE, 0, envblock, cwd,
                         &si, &pi);
    free(cmdline);
    free(envblock);
    if (!ok)
        return -EIO; /* GetLastError() doesn't map cleanly to errno */
    CloseHandle(pi.hThread);
    proc_table_add(pi.dwProcessId, pi.hProcess);
    *out_pid = (int)pi.dwProcessId;
    return 0;
}

static int libc_process_wait(JSLibcPal *opaque, int pid, int options, int *pstatus)
{
    HANDLE h;
    DWORD wr, exit_code;
    int owned;

    (void)opaque;

    h = proc_table_find((DWORD)pid);
    owned = (h != NULL);
    if (!owned) {
        h = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
        if (!h)
            return -ESRCH;
    }
    wr = WaitForSingleObject(h, (options & JS_LIBC_WNOHANG) ? 0 : INFINITE);
    if (wr == WAIT_TIMEOUT) {
        if (!owned)
            CloseHandle(h);
        return 0;
    }
    if (wr != WAIT_OBJECT_0) {
        if (!owned)
            CloseHandle(h);
        return -EINVAL;
    }
    if (!GetExitCodeProcess(h, &exit_code))
        exit_code = (DWORD)-1;
    *pstatus = (int)((exit_code & 0xff) << 8);
    if (owned)
        proc_table_remove((DWORD)pid);
    CloseHandle(h);
    return pid;
}

static int libc_process_kill(JSLibcPal *opaque, int pid, int sig)
{
    HANDLE h;
    int owned;
    BOOL ok;

    (void)opaque;
    (void)sig; /* no Windows equivalent of arbitrary signal delivery */

    h = proc_table_find((DWORD)pid);
    owned = (h != NULL);
    if (!owned) {
        h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (!h)
            return -ESRCH;
    }
    ok = TerminateProcess(h, 1);
    if (!owned)
        CloseHandle(h);
    return ok ? 0 : -EINVAL;
}

#else /* !_WIN32 */

#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

/* execvpe is not available on non GNU systems */
static int my_execvpe(const char *filename, char **argv, char **envp)
{
    char *path, *p, *p_next, *p1;
    char buf[PATH_MAX];
    size_t filename_len, path_len;
    BOOL eacces_error;

    filename_len = strlen(filename);
    if (filename_len == 0) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(filename, '/'))
        return execve(filename, argv, envp);

    path = getenv("PATH");
    if (!path)
        path = (char *)"/bin:/usr/bin";
    eacces_error = FALSE;
    for (p = path; p != NULL; p = p_next) {
        p1 = strchr(p, ':');
        if (!p1) {
            p_next = NULL;
            path_len = strlen(p);
        } else {
            p_next = p1 + 1;
            path_len = p1 - p;
        }
        /* path too long */
        if ((path_len + 1 + filename_len + 1) > PATH_MAX)
            continue;
        memcpy(buf, p, path_len);
        buf[path_len] = '/';
        memcpy(buf + path_len + 1, filename, filename_len);
        buf[path_len + 1 + filename_len] = '\0';

        execve(buf, argv, envp);

        switch (errno) {
        case EACCES:
            eacces_error = TRUE;
            break;
        case ENOENT:
        case ENOTDIR:
            break;
        default:
            return -1;
        }
    }
    if (eacces_error)
        errno = EACCES;
    return -1;
}

static int libc_process_spawn(JSLibcPal *opaque, const char *file,
                               char *const argv[], char *const envp[],
                               const char *cwd, const int std_fds[3],
                               int use_path, uint32_t uid, uint32_t gid,
                               int *out_pid)
{
    int pid, i;

    (void)opaque;

    pid = fork();
    if (pid < 0)
        return -errno;
    if (pid == 0) {
        /* child */
        for (i = 0; i < 3; i++) {
            if (std_fds[i] != i) {
                if (dup2(std_fds[i], i) < 0)
                    _exit(127);
            }
        }
#if defined(HAVE_CLOSEFROM)
        /* closefrom() is available on many recent unix systems:
           Linux with glibc 2.34+, Solaris 9+, FreeBSD 7.3+,
           NetBSD 3.0+, OpenBSD 3.5+.
           Linux with the musl libc and macOS don't have it.
         */
        closefrom(3);
#else
        {
            /* Close the file handles manually, limit to 1024 to avoid
               costly loop on linux Alpine where sysconf(_SC_OPEN_MAX)
               returns a huge value 1048576.
               Patch inspired by nicolas-duteil-nova. See also:
               https://stackoverflow.com/questions/73229353/
               https://stackoverflow.com/questions/899038/#918469
             */
            int fd_max = min_int(sysconf(_SC_OPEN_MAX), 1024);
            for (i = 3; i < fd_max; i++)
                close(i);
        }
#endif
        if (cwd) {
            if (chdir(cwd) < 0)
                _exit(127);
        }
        if (gid != (uint32_t)-1) {
            if (setgid(gid) < 0)
                _exit(127);
        }
        if (uid != (uint32_t)-1) {
            if (setuid(uid) < 0)
                _exit(127);
        }
        if (!file)
            file = argv[0];
        if (use_path)
            my_execvpe(file, (char **)argv, (char **)envp);
        else
            execve(file, (char *const *)argv, (char *const *)envp);
        _exit(127);
    }
    *out_pid = pid;
    return 0;
}

static int libc_process_wait(JSLibcPal *opaque, int pid, int options, int *pstatus)
{
    int ret;

    (void)opaque;
    ret = waitpid(pid, pstatus, options);
    if (ret < 0)
        return -errno;
    return ret;
}

static int libc_process_kill(JSLibcPal *opaque, int pid, int sig)
{
    (void)opaque;
    if (kill(pid, sig) < 0)
        return -errno;
    return 0;
}

#endif /* !_WIN32 */

const JSLibcPalFunctions js_libc_pal = {
    .process_spawn = libc_process_spawn,
    .process_wait = libc_process_wait,
    .process_kill = libc_process_kill,
};
