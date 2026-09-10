/*
 * QuickJS libc host-binding: ucrt POSIX-name compatibility shim
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
#ifndef QUICKJS_LIBC_WIN32_COMPAT_H
#define QUICKJS_LIBC_WIN32_COMPAT_H

/* Included only under native MSVC/ucrt (_MSC_VER); MinGW's CRT already
   provides all of the POSIX names below natively. Satisfies the existing
   `#if defined(_WIN32)`/unguarded POSIX-named calls in quickjs-libc.c,
   qjsc.c and run-test262.c against the ucrt names/semantics, rather than
   rewriting those call sites. */

#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utime.h> /* ucrt has no top-level <utime.h>; struct _utimbuf/_utime live here */

typedef long ssize_t;

/* ucrt's <sys/stat.h> only defines these under _CRT_INTERNAL_NONSTDC_NAMES,
   which isn't reliably on for our build flags; define them unconditionally
   from the always-available "_S_..." spellings instead. S_IFBLK has no ucrt
   equivalent (no block devices) -- use the same placeholder bit pattern
   MinGW's sys/stat.h uses, for consistency with the already-verified build. */
#ifndef S_IFMT
#define S_IFMT _S_IFMT
#endif
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_IFCHR
#define S_IFCHR _S_IFCHR
#endif
#ifndef S_IFREG
#define S_IFREG _S_IFREG
#endif
#ifndef S_IFIFO
#define S_IFIFO _S_IFIFO
#endif
#ifndef S_IFBLK
#define S_IFBLK 0x6000
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#define popen  _popen
#define pclose _pclose

#define access _access
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 0 /* ucrt has no "executable" bit; treat X_OK as F_OK */
#endif
#ifndef F_OK
#define F_OK 0
#endif

#define isatty  _isatty
#define fileno  _fileno
#define unlink  _unlink
#define dup     _dup
#define dup2    _dup2
#define getpid  _getpid
#define getcwd  _getcwd
#define chdir   _chdir

/* the `#if defined(_WIN32)` branch in js_os_mkdir() already calls the
   one-arg form (no mode) -- ucrt spells that "_mkdir". */
#define mkdir(path) _mkdir(path)

/*----------------------------------------------------------------------*/
/* dirent.h emulation, backed by FindFirstFileA/FindNextFileA */

struct dirent {
    char d_name[260 /* MAX_PATH */];
};

typedef struct DIR {
    struct _finddata_t fdata;
    intptr_t handle;
    int first;
    struct dirent ent;
} DIR;

static inline DIR *opendir(const char *name)
{
    DIR *d;
    char pattern[260 + 2];
    size_t len;

    d = (DIR *)malloc(sizeof(DIR));
    if (!d)
        return NULL;
    len = strlen(name);
    if (len + 3 > sizeof(pattern)) {
        free(d);
        return NULL;
    }
    memcpy(pattern, name, len);
    if (len > 0 && pattern[len - 1] != '/' && pattern[len - 1] != '\\')
        pattern[len++] = '/';
    pattern[len++] = '*';
    pattern[len] = '\0';
    d->handle = _findfirst(pattern, &d->fdata);
    if (d->handle == -1) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static inline struct dirent *readdir(DIR *d)
{
    if (!d->first) {
        if (_findnext(d->handle, &d->fdata) != 0)
            return NULL;
    }
    d->first = 0;
    strncpy(d->ent.d_name, d->fdata.name, sizeof(d->ent.d_name) - 1);
    d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
    return &d->ent;
}

static inline int closedir(DIR *d)
{
    int ret = 0;
    if (d->handle != -1)
        ret = _findclose(d->handle);
    free(d);
    return ret;
}

/*----------------------------------------------------------------------*/
/* ftw.h emulation, backed by the opendir/readdir/stat above -- just
   enough of the <ftw.h> contract for run-test262.c's directory walk
   (recursive, calls fn(path, &statbuf, flag) for every entry). */

#ifndef FTW_F
#define FTW_F 0
#endif
#ifndef FTW_D
#define FTW_D 1
#endif

static inline int ftw(const char *dir, int (*fn)(const char *, const struct stat *, int),
                       int nopenfd)
{
    DIR *d;
    struct dirent *ent;
    char path[1024];
    struct stat st;
    int ret;

    d = opendir(dir);
    if (!d)
        return -1;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            ret = fn(path, &st, FTW_D);
            if (ret) {
                closedir(d);
                return ret;
            }
            ret = ftw(path, fn, nopenfd);
            if (ret) {
                closedir(d);
                return ret;
            }
        } else {
            ret = fn(path, &st, FTW_F);
            if (ret) {
                closedir(d);
                return ret;
            }
        }
    }
    closedir(d);
    return 0;
}

#endif /* QUICKJS_LIBC_WIN32_COMPAT_H */
