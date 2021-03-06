/*
 * This file is part of the Soletta Project
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sol-mainloop.h"
#include "sol-platform-impl.h"
#include "sol-platform-linux.h"
#include "sol-platform.h"
#include "sol-vector.h"
#include "sol-util.h"

struct sol_platform_linux_fork_run {
    pid_t pid;
    void (*on_child_exit)(void *data, uint64_t pid, int status);
    const void *data;
    struct sol_child_watch *watch;
};

static struct sol_ptr_vector fork_runs = SOL_PTR_VECTOR_INIT;

static uint16_t
find_handle(const struct sol_platform_linux_fork_run *handle)
{
    struct sol_platform_linux_fork_run *itr = NULL;
    uint16_t i;

    errno = ENOENT;
    SOL_NULL_CHECK(handle, UINT16_MAX);

    SOL_PTR_VECTOR_FOREACH_IDX (&fork_runs, itr, i) {
        if (itr == handle) {
            errno = 0;
            return i;
        }
    }

    return UINT16_MAX;
}

static void
on_child(void *data, uint64_t pid, int status)
{
    struct sol_platform_linux_fork_run *handle = data;
    uint16_t i;

    i = find_handle(handle);
    SOL_INT_CHECK(i, == UINT16_MAX);
    sol_ptr_vector_del(&fork_runs, i);

    if (handle->on_child_exit)
        handle->on_child_exit((void *)handle->data, pid, status);
    free(handle);
}

SOL_API struct sol_platform_linux_fork_run *
sol_platform_linux_fork_run(void (*on_fork)(void *data), void (*on_child_exit)(void *data, uint64_t pid, int status), const void *data)
{
    pid_t pid;
    int pfds[2];

    errno = EINVAL;
    SOL_NULL_CHECK(on_fork, NULL);

    if (pipe(pfds) < 0) {
        SOL_WRN("could not create pipe: %s", sol_util_strerrora(errno));
        return NULL;
    }

    pid = fork();
    if (pid == 0) {
        sigset_t emptyset;
        char msg;

        sigemptyset(&emptyset);
        sigprocmask(SIG_SETMASK, &emptyset, NULL);

        close(pfds[1]);
        while (read(pfds[0], &msg, 1) < 0) {
            if (errno == EINTR)
                continue;
            else {
                SOL_WRN("failed to read from pipe: %s", sol_util_strerrora(errno));
                close(pfds[0]);
                sol_platform_linux_fork_run_exit(EXIT_FAILURE);
            }
        }
        close(pfds[0]);

        errno = 0;
        on_fork((void *)data);
        sol_platform_linux_fork_run_exit(EXIT_SUCCESS);
    } else if (pid < 0) {
        int errno_bkp = errno;
        close(pfds[0]);
        close(pfds[1]);
        errno = errno_bkp;
        SOL_WRN("could not fork: %s", sol_util_strerrora(errno));
        return NULL;
    } else {
        struct sol_platform_linux_fork_run *handle;
        char msg = 0xff;
        int status = 0;

        handle = malloc(sizeof(*handle));
        SOL_NULL_CHECK_GOTO(handle, error_malloc);
        handle->pid = pid;
        handle->on_child_exit = on_child_exit;
        handle->data = data;
        handle->watch = sol_child_watch_add(pid, on_child, handle);
        SOL_NULL_CHECK_GOTO(handle, error_watch);

        status = sol_ptr_vector_append(&fork_runs, handle);
        SOL_INT_CHECK_GOTO(status, < 0, error_append);

        close(pfds[0]);
        while (write(pfds[1], &msg, 1) < 0) {
            if (errno == EINTR)
                continue;
            else {
                SOL_WRN("failed to write to pipe: %s", sol_util_strerrora(errno));
                status = errno;
                close(pfds[1]);
                errno = status;
                goto error_wpipe;
            }
        }
        close(pfds[1]);

        errno = 0;
        return handle;

error_wpipe:
        status = errno;
        sol_ptr_vector_del(&fork_runs, sol_ptr_vector_get_len(&fork_runs) - 1);
        errno = status;
error_append:
        status = errno;
        sol_child_watch_del(handle->watch);
        errno = status;
error_watch:
        status = errno;
        free(handle);
        errno = status;
error_malloc:
        status = errno;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = status;
        return NULL;
    }
}

SOL_API int
sol_platform_linux_fork_run_send_signal(struct sol_platform_linux_fork_run *handle, int sig)
{
    SOL_INT_CHECK(find_handle(handle), == UINT16_MAX, -errno);
    return kill(handle->pid, sig) == 0;
}

SOL_API int
sol_platform_linux_fork_run_stop(struct sol_platform_linux_fork_run *handle)
{
    int errno_bkp, status = 0;
    uint16_t i;

    i = find_handle(handle);
    SOL_INT_CHECK(i, == UINT16_MAX, -errno);
    sol_ptr_vector_del(&fork_runs, i);

    sol_child_watch_del(handle->watch);

    kill(handle->pid, SIGTERM);
    errno = 0;
    while (waitpid(handle->pid, &status, 0) < 0) {
        if (errno == EINTR) {
            errno = 0;
            continue;
        }

        SOL_WRN("waitpid(%" PRIu64 "): %s",
            (uint64_t)handle->pid, sol_util_strerrora(errno));

        if (errno == ECHILD)
            errno = 0; /* weird, but let's assume success */
        break;
    }

    errno_bkp = errno;

    if (handle->on_child_exit)
        handle->on_child_exit((void *)handle->data, handle->pid,
            WIFEXITED(status) ? WEXITSTATUS(status) : status);
    free(handle);

    errno = errno_bkp;
    return 0;
}

SOL_API uint64_t
sol_platform_linux_fork_run_get_pid(const struct sol_platform_linux_fork_run *handle)
{
    SOL_INT_CHECK(find_handle(handle), == UINT16_MAX, UINT64_MAX);
    return handle->pid;
}

SOL_API void
sol_platform_linux_fork_run_exit(int status)
{
    _exit(status);
}

int
sol_platform_impl_get_os_version(char **version)
{
    int r;
    struct utsname hostinfo;

    SOL_NULL_CHECK(version, -EINVAL);

    r = uname(&hostinfo);
    SOL_INT_CHECK(r, == -1, -errno);

    *version = strdup(hostinfo.release);
    if (!*version)
        return -ENOMEM;

    return 0;
}
