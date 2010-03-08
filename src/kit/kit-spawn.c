/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * kit-spawn.c : Spawn utilities
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <kit/kit.h>
#include "kit-test.h"


/**
 * SECTION:kit-spawn
 * @title: Spawn utilities
 * @short_description: Spawn utilities
 *
 * Various spawn utilities.
 **/

static kit_bool_t
set_close_on_exec (int fd, void *data)
{
        if (fd >= (int) data) {
                if (fcntl (fd, F_SETFD, FD_CLOEXEC) != 0 && errno != EBADF) {
                        return FALSE;
                }
        }

        return TRUE;
}

static kit_bool_t
_fdwalk (kit_bool_t (*callback)(int fd, void *user_data), void *user_data)
{
        int fd;
        int max_fd;

        kit_return_val_if_fail (callback != NULL, FALSE);

        max_fd = sysconf (_SC_OPEN_MAX);
        for (fd = 0; fd < max_fd; fd++) {
                if (!callback (fd, user_data))
                        return FALSE;
        }

        return TRUE;
}

static int
_sane_dup2 (int fd1, int fd2)
{
        int ret;
        
again:
        ret = dup2 (fd1, fd2);
        if (ret < 0 && errno == EINTR)
                goto again;
        
        return ret;
}

static ssize_t
_read_from (int fd, char **out_string)
{
        char buf[4096];
        ssize_t num_read;

again:
        num_read = read (fd, buf, sizeof (buf) - 1);
        if (num_read == -1) {
                if (errno == EINTR)
                        goto again;
                else
                        goto out;
        }

        if (num_read > 0) {
                char *s;

                buf[num_read] = '\0';

                s = kit_str_append (*out_string, buf);
                if (s == NULL) {
                        errno = ENOMEM;
                        num_read = -1;
                        goto out;
                }
                *out_string = s;

                //kit_debug ("fd=%d read %d bytes: '%s'", fd, num_read, buf);
        }

out:
        return num_read;
}

static ssize_t
_write_to (int fd, char *str)
{
        ssize_t num_written;

again:
        num_written = write (fd, str, strlen (str));
        if (num_written == -1) {
                if (errno == EINTR)
                        goto again;
                else
                        goto out;
        }

        //kit_debug ("Wrote %d bytes from '%s'", num_written, str);

out:
        return num_written;
}

/**
 * kit_spawn_sync:
 * @working_directory: Working directory for child or #NULL to inherit parents
 * @flags: A combination of flags from #KitSpawnFlags
 * @argv: #NULL terminated argument vector
 * @envp: #NULL terminated environment or #NULL to inherit parents;
 * @stdinp: String to write to stdin of child or #NULL
 * @stdoutp: Return location for stdout from child or #NULL. Free with kit_free().
 * @stderrp: Return location for stderr from child or #NULL. Free with kit_free().
 * @out_exit_status: Return location for exit status
 *
 * Executes a child process and waits for the child process to exit
 * before returning. The exit status of the child is stored in
 * @out_exit_status as it would be returned by waitpid(); standard
 * UNIX macros such as WIFEXITED() and WEXITSTATUS() must be used to
 * evaluate the exit status.
 *
 * Returns: #TRUE if the child was executed; #FALSE if an error
 * occured and errno will be set.
 */
kit_bool_t
kit_spawn_sync (const char     *working_directory,
                KitSpawnFlags   flags,
                char          **argv,
                char          **envp,
                char           *stdinp,
                char          **stdoutp,
                char          **stderrp,
                int            *out_exit_status)
{
        kit_bool_t ret;
        pid_t pid;
        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        int stderr_pipe[2] = {-1, -1};

        ret = FALSE;
        pid = -1;

        kit_return_val_if_fail (argv != NULL, FALSE);
        kit_return_val_if_fail (out_exit_status != NULL, FALSE);
        kit_return_val_if_fail (! ((flags & KIT_SPAWN_CHILD_INHERITS_STDIN) && stdinp != NULL), FALSE);
        kit_return_val_if_fail (! ((flags & KIT_SPAWN_STDOUT_TO_DEV_NULL) && stdoutp != NULL), FALSE);
        kit_return_val_if_fail (! ((flags & KIT_SPAWN_STDERR_TO_DEV_NULL) && stderrp != NULL), FALSE);

        if (stdoutp != NULL)
                *stdoutp = NULL;
        if (stderrp != NULL)
                *stderrp = NULL;

        if (stdinp != NULL) {
                if (pipe (stdin_pipe) != 0) {
                        goto out;
                }
        }

        if (stdoutp != NULL) {
                if (pipe (stdout_pipe) != 0) {
                        goto out;
                }
        }

        if (stderrp != NULL) {
                if (pipe (stderr_pipe) != 0) {
                        goto out;
                }
        }

        pid = fork ();
        if (pid == -1) {
                goto out;
        }

        if (pid == 0) {
                int fd_null = -1;

                /* child */

                if ( (!(flags & KIT_SPAWN_CHILD_INHERITS_STDIN)) ||
                     (flags & KIT_SPAWN_STDOUT_TO_DEV_NULL) ||
                     (flags & KIT_SPAWN_STDERR_TO_DEV_NULL)) {
                        fd_null = open ("/dev/null", O_RDONLY);
                        if (fd_null < 0) {
                                exit (128 + errno);
                        }
                }

                signal (SIGPIPE, SIG_DFL);

                /* close unused ends */
                if (stdin_pipe[1] != -1) {
                        close (stdin_pipe[1]);
                }
                if (stdout_pipe[0] != -1) {
                        close (stdout_pipe[0]);
                }
                if (stderr_pipe[0] != -1) {
                        close (stderr_pipe[0]);
                }

                /* close all open file descriptors of child except stdin, stdout, stderr */
                _fdwalk (set_close_on_exec, (void *) 3);
                
                /* change working directory */
                if (working_directory != NULL) {
                        if (chdir (working_directory) != 0) {
                                exit (128 + errno);
                        }
                }

                /* set stdinp, stdoutp and stderrp */

                if (stdinp != NULL) {
                        if (_sane_dup2 (stdin_pipe[0], 0) < 0) {
                                exit (128 + errno);
                        }
                } else if (! (flags & KIT_SPAWN_CHILD_INHERITS_STDIN)) {
                        if (_sane_dup2 (fd_null, 0) < 0) {
                                exit (128 + errno);
                        }
                }

                if (stdoutp != NULL) {
                        if (_sane_dup2 (stdout_pipe[1], 1) < 0) {
                                exit (128 + errno);
                        }
                } else if (flags & KIT_SPAWN_STDOUT_TO_DEV_NULL) {
                        if (_sane_dup2 (fd_null, 1) < 0) {
                                exit (128 + errno);
                        }
                }

                if (stderrp != NULL) {
                        if (_sane_dup2 (stderr_pipe[1], 2) < 0) {
                                exit (128 + errno);
                        }
                } else if (flags & KIT_SPAWN_STDERR_TO_DEV_NULL) {
                        if (_sane_dup2 (fd_null, 2) < 0) {
                                exit (128 + errno);
                        }
                }

                if (fd_null != -1)
                        close (fd_null);

                /* finally, execute the child */
                if (envp != NULL) {
                        if (execve (argv[0], argv, envp) == -1) {
                                exit (128 + errno);
                        }
                } else {
                        if (execv (argv[0], argv) == -1) {
                                exit (128 + errno);
                        }
                }

        } else {
                char *wp;

                /* parent */

                /* closed unused ends */
                if (stdin_pipe[0] != -1) {
                        close (stdin_pipe[0]);
                }
                if (stdout_pipe[1] != -1) {
                        close (stdout_pipe[1]);
                }
                if (stderr_pipe[1] != -1) {
                        close (stderr_pipe[1]);
                }

                wp = stdinp;

                while (stdin_pipe[1] != -1 || stdout_pipe[0] != -1 || stderr_pipe[0] != -1) {
                        int ret;
                        ssize_t num_read;
                        ssize_t num_written;
                        int max;
                        fd_set read_fds;
                        fd_set write_fds;
                        
                        FD_ZERO (&read_fds);
                        FD_ZERO (&write_fds);
                        if (stdin_pipe[1] != -1) {
                                FD_SET (stdin_pipe[1], &write_fds);
                        }
                        if (stdout_pipe[0] != -1) {
                                FD_SET (stdout_pipe[0], &read_fds);
                        }
                        if (stderr_pipe[0] != -1) {
                                FD_SET (stderr_pipe[0], &read_fds);
                        }
                        
                        max = stdin_pipe[1];
                        if (stdout_pipe[0] > max)
                                max = stdout_pipe[0];
                        if (stderr_pipe[0] > max)
                                max = stderr_pipe[0];
                        
                        ret = select (max + 1, 
                                      &read_fds, 
                                      &write_fds, 
                                      NULL, 
                                      NULL);
                        
                        if (ret < 0 && errno != EINTR) {
                                goto out;
                        }
                        
                        if (stdin_pipe[1] != -1) {
                                num_written = _write_to (stdin_pipe[1], wp);
                                
                                if (num_written == -1)  {
                                        goto out;
                                }
                                
                                wp += num_written;
                                if (*wp == '\0') {
                                        close (stdin_pipe[1]);
                                        stdin_pipe[1] = -1;
                                }
                        }
                        
                        if (stdout_pipe[0] != -1) {
                                num_read = _read_from (stdout_pipe[0], stdoutp);
                                if (num_read == 0) {
                                        close (stdout_pipe[0]);
                                        stdout_pipe[0] = -1;
                                } else if (num_read == -1)  {
                                        goto out;
                                }
                        }
                        
                        if (stderr_pipe[0] != -1) {
                                num_read = _read_from (stderr_pipe[0], stderrp);
                                if (num_read == 0) {
                                        close (stderr_pipe[0]);
                                        stderr_pipe[0] = -1;
                                } else if (num_read == -1)  {
                                        goto out;
                                }
                        }
                }

                if (waitpid (pid, out_exit_status, 0) == -1) {
                        goto out;
                }
                pid = -1;
        }

        //kit_debug ("exit %d", WEXITSTATUS (*out_exit_status));

        if (WEXITSTATUS (*out_exit_status) < 128) {
                ret = TRUE;
        } else {
                ret = FALSE;
                errno = WEXITSTATUS (*out_exit_status) - 128;
        }

out:
        if (pid != -1) {
                kill (pid, SIGKILL);
                waitpid (pid, out_exit_status, 0);                
        }

        if (stdin_pipe[1] != -1)
                close (stdin_pipe[1]);
        if (stdout_pipe[0] != -1)
                close (stdout_pipe[0]);
        if (stderr_pipe[0] != -1)
                close (stderr_pipe[0]);

        if (!ret) {
                if (stdoutp != NULL) {
                        kit_free (*stdoutp);
                        *stdoutp = NULL;
                }
                if (stderrp != NULL) {
                        kit_free (*stderrp);
                        *stderrp = NULL;
                }
        }

        return ret;

}


#ifdef KIT_BUILD_TESTS

static kit_bool_t
_run_test (void)
{
        char path[] = "/tmp/kit-spawn-test";
        char *script1 = 
                "#!/bin/sh"                      "\n"
                "echo \"Hello World\""           "\n"
                "echo \"Goodbye World\" 1>&2"    "\n"
                "exit 42"                        "\n";
        char *script2 = 
                "#!/bin/sh"  "\n"
                "exit 43"    "\n";
        char *script3 = 
                "#!/bin/sh"                       "\n"
                "echo -n \"$KIT_TEST_VAR\""       "\n"
                "exit 0"                          "\n";
        char *script4 = 
                "#!/bin/sh"                                "\n"
                "if [ \"x$KIT_TEST_VAR\" = \"x\" ] ; then" "\n"
                "  exit 0"                                 "\n"
                "fi"                                       "\n"
                "exit 1"                                   "\n";
        char *script4b = 
                "#!/bin/sh"                                "\n"
                "/bin/env > /tmp/food2"                     "\n"
                "if [ \"x$KIT_TEST_VAR\" = \"xfoobar2\" ] ; then" "\n"
                "  exit 0"                                 "\n"
                "fi"                                       "\n"
                "exit 1"                                   "\n";
        char *script5 = 
                "#!/bin/sh"                                "\n"
                "pwd"                                      "\n"
                "exit 0"                                   "\n";
        char *script6 = 
                "#!/bin/sh"                                "\n"
                "read value"                               "\n"
                "echo -n \"$value\""                       "\n"
                "echo -n \" \""                            "\n"
                "read value"                               "\n"
                "echo -n \"$value\""                       "\n"
                "exit 0"                                   "\n";
        char *argv[] = {"/tmp/kit-spawn-test", NULL};
        char *stdoutp;
        char *stderrp;
        int exit_status;
        struct stat statbuf;

        /* script echoing to stdout and stderr */
        if (kit_file_set_contents (path, 0700, script1, strlen (script1))) {
                if (kit_spawn_sync ("/",
                                    0,
                                    argv,
                                    NULL,
                                    NULL,
                                    &stdoutp,
                                    &stderrp,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 42);
                        kit_assert (stdoutp != NULL && strcmp (stdoutp, "Hello World\n") == 0);
                        kit_assert (stderrp != NULL && strcmp (stderrp, "Goodbye World\n") == 0);
                        kit_free (stdoutp);
                        kit_free (stderrp);
                }

                if (kit_spawn_sync ("/",
                                    0,
                                    argv,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 42);
                }

                kit_assert (unlink (path) == 0);
        }

        /* silent script */
        if (kit_file_set_contents (path, 0700, script2, strlen (script2))) {
                if (kit_spawn_sync ("/",
                                    0,
                                    argv,
                                    NULL,
                                    NULL,
                                    &stdoutp,
                                    &stderrp,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 43);
                        kit_assert (stdoutp == NULL);
                        kit_assert (stderrp == NULL);
                }

                kit_assert (unlink (path) == 0);
        }

        /* check environment is set */
        if (kit_file_set_contents (path, 0700, script3, strlen (script3))) {
                char *envp[] = {"KIT_TEST_VAR=some_value", NULL};

                if (kit_spawn_sync ("/",
                                    0,
                                    argv,
                                    envp,
                                    NULL,
                                    &stdoutp,
                                    NULL,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 0);
                        kit_assert (stdoutp != NULL && strcmp (stdoutp, "some_value") == 0);
                        kit_free (stdoutp);
                }

                kit_assert (unlink (path) == 0);
        }

        /* check environment is replaced */
        if (kit_file_set_contents (path, 0700, script4, strlen (script4))) {
                char *envp[] = {NULL};

                kit_assert (setenv ("KIT_TEST_VAR", "foobar", 1) == 0);

                if (kit_spawn_sync ("/",
                                    0,
                                    argv,
                                    envp,
                                    NULL,
                                    NULL,
                                    NULL,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 0);
                }

                kit_assert (unlink (path) == 0);
                kit_assert (unsetenv ("KIT_TEST_VAR") == 0);
        }

        /* check environment is inherited */
        if (kit_file_set_contents (path, 0700, script4b, strlen (script4b))) {

                kit_assert (setenv ("KIT_TEST_VAR", "foobar2", 1) == 0);

                if (kit_spawn_sync ("/",
                                    0,
                                    argv,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 0);
                }

                kit_assert (unlink (path) == 0);
                kit_assert (unsetenv ("KIT_TEST_VAR") == 0);
        }

        /* check working directory */
        if (kit_file_set_contents (path, 0700, script5, strlen (script5))) {
                kit_assert (stat ("/tmp", &statbuf) == 0 && S_ISDIR (statbuf.st_mode));
                if (kit_spawn_sync ("/tmp",
                                    0,
                                    argv,
                                    NULL,
                                    NULL,
                                    &stdoutp,
                                    NULL,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 0);
                        kit_assert (stdoutp != NULL && strcmp (stdoutp, "/tmp\n") == 0);
                        kit_free (stdoutp);
                }

                kit_assert (stat ("/usr", &statbuf) == 0 && S_ISDIR (statbuf.st_mode));
                if (kit_spawn_sync ("/usr",
                                    0,
                                    argv,
                                    NULL,
                                    NULL,
                                    &stdoutp,
                                    NULL,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 0);
                        kit_assert (stdoutp != NULL && strcmp (stdoutp, "/usr\n") == 0);
                        kit_free (stdoutp);
                }

                kit_assert (unlink (path) == 0);
        }

        /* check bogus working directory */
        kit_assert (stat ("/org/freedesktop/PolicyKit/bogus-fs-path", &statbuf) != 0);
        kit_assert (kit_spawn_sync ("/org/freedesktop/PolicyKit/bogus-fs-path",
                                    0,
                                    argv,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    &exit_status) == FALSE && 
                    (errno == ENOENT || errno == ENOMEM));

        /* check for writing to stdin */
        if (kit_file_set_contents (path, 0700, script6, strlen (script6))) {
                if (kit_spawn_sync (NULL,
                                    0,
                                    argv,
                                    NULL,
                                    "foobar0\nfoobar1",
                                    &stdoutp,
                                    NULL,
                                    &exit_status)) {
                        kit_assert (WEXITSTATUS (exit_status) == 0);
                        kit_assert (stdoutp != NULL && strcmp (stdoutp, "foobar0 foobar1") == 0);
                        kit_free (stdoutp);
                }

                kit_assert (unlink (path) == 0);
        }

        return TRUE;
}

KitTest _test_spawn = {
        "kit_spawn",
        NULL,
        NULL,
        _run_test
};

#endif /* KIT_BUILD_TESTS */
