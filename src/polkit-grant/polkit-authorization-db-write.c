/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-authorization-db.c : Represents the authorization database
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>

#include <glib.h>

#include <polkit/polkit-debug.h>
#include <polkit/polkit-authorization-db.h>
#include <polkit/polkit-utils.h>
#include <polkit/polkit-private.h>

/**
 * SECTION:polkit-authorization-db
 **/


static polkit_bool_t
_write_to_fd (int fd, const char *str, ssize_t str_len)
{
        polkit_bool_t ret;
        ssize_t written;

        ret = FALSE;

        written = 0;
        while (written < str_len) {
                ssize_t ret;
                ret = write (fd, str + written, str_len - written);
                if (ret < 0) {
                        if (errno == EAGAIN || errno == EINTR) {
                                continue;
                        } else {
                                goto out;
                        }
                }
                written += ret;
        }

        ret = TRUE;

out:
        return ret;
}

polkit_bool_t 
_polkit_authorization_db_auth_file_add (polkit_bool_t transient, uid_t uid, char *str_to_add)
{
        int fd;
        char *contents;
        gsize contents_size;
        char *path;
        char *path_tmp;
        GError *error;
        polkit_bool_t ret;
        struct stat statbuf;
        struct passwd *pw;
        const char *root;
        char *newline = "\n";

        if (transient)
                root = PACKAGE_LOCALSTATE_DIR "/run/PolicyKit";
        else
                root = PACKAGE_LOCALSTATE_DIR "/lib/PolicyKit";

        ret = FALSE;
        path = NULL;
        path_tmp = NULL;
        contents = NULL;

        pw = getpwuid (uid);
        if (pw == NULL) {
                g_warning ("cannot lookup user name for uid %d\n", uid);
                goto out;
        }

        path = g_strdup_printf ("%s/user-%s.auths", root, pw->pw_name);
        path_tmp = g_strdup_printf ("%s.XXXXXX", path);

        if (stat (path, &statbuf) != 0 && errno == ENOENT) {
                //fprintf (stderr, "path=%s does not exist (egid=%d): %m!\n", path, getegid ());

                g_free (path_tmp);
                path_tmp = path;
                path = NULL;

                /* Write a nice blurb if we're creating the file for the first time */

                contents = g_strdup_printf (
                        "# This file lists authorizations for user %s\n"
                        "%s"
                        "# \n"
                        "# File format may change at any time; do not rely on it. To manage\n"
                        "# authorizations use polkit-auth(1) instead.\n"
                        "\n",
                        pw->pw_name,
                        transient ? "# (these are temporary and will be removed on the next system boot)\n" : "");
                contents_size = strlen (contents);
        } else {
                error = NULL;
                if (!g_file_get_contents (path, &contents, &contents_size, &error)) {
                        g_warning ("Cannot read authorizations file %s: %s", path, error->message);
                        g_error_free (error);
                        goto out;
                }
        }

        if (path != NULL) {
                fd = mkstemp (path_tmp);
                if (fd < 0) {
                        fprintf (stderr, "Cannot create file '%s': %m\n", path_tmp);
                        goto out;
                }
                if (fchmod (fd, 0464) != 0) {
                        fprintf (stderr, "Cannot change mode for '%s' to 0460: %m\n", path_tmp);
                        close (fd);
                        unlink (path_tmp);
                        goto out;
                }
        } else {
                fd = open (path_tmp, O_RDWR|O_CREAT, 0464);
                if (fd < 0) {
                        fprintf (stderr, "Cannot create file '%s': %m\n", path_tmp);
                        goto out;
                }
        }

        if (!_write_to_fd (fd, contents, contents_size)) {
                g_warning ("Cannot write to temporary authorizations file %s: %m", path_tmp);
                close (fd);
                if (unlink (path_tmp) != 0) {
                        g_warning ("Cannot unlink %s: %m", path_tmp);
                }
                goto out;
        }
        if (!_write_to_fd (fd, str_to_add, strlen (str_to_add))) {
                g_warning ("Cannot write to temporary authorizations file %s: %m", path_tmp);
                close (fd);
                if (unlink (path_tmp) != 0) {
                        g_warning ("Cannot unlink %s: %m", path_tmp);
                }
                goto out;
        }
        if (!_write_to_fd (fd, newline, 1)) {
                g_warning ("Cannot write to temporary authorizations file %s: %m", path_tmp);
                close (fd);
                if (unlink (path_tmp) != 0) {
                        g_warning ("Cannot unlink %s: %m", path_tmp);
                }
                goto out;
        }
        close (fd);

        if (path != NULL) {
                if (rename (path_tmp, path) != 0) {
                        g_warning ("Cannot rename %s to %s: %m", path_tmp, path);
                        if (unlink (path_tmp) != 0) {
                                g_warning ("Cannot unlink %s: %m", path_tmp);
                        }
                        goto out;
                }
        }

        /* trigger a reload */
        if (utimes (PACKAGE_LOCALSTATE_DIR "/lib/misc/PolicyKit.reload", NULL) != 0) {
                g_warning ("Error updating access+modification time on file '%s': %m\n", 
                           PACKAGE_LOCALSTATE_DIR "/lib/misc/PolicyKit.reload");
        }

        ret = TRUE;

out:
        if (contents != NULL)
                g_free (contents);
        if (path != NULL)
                g_free (path);
        if (path_tmp != NULL)
                g_free (path_tmp);
        return ret;
}

/* returns -1 on error */
static int
_write_constraints (char *buf, size_t buf_size, PolKitAuthorizationConstraint **constraints)
{
        unsigned int n;
        unsigned int m;

        kit_return_val_if_fail (constraints != NULL, 0);

        for (n = 0, m = 0; constraints[n] != NULL; n++) {
                PolKitAuthorizationConstraint *c;
                const char *key;
                char value[1024];

                c = constraints[n];

                key = "constraint";

                if (polkit_authorization_constraint_to_string (c, value, sizeof (value)) >= sizeof (value)) {
                        kit_warning ("Constraint %d is too large!", n);
                        m = -1;
                        goto out;
                }

                if (m < buf_size)
                        buf[m] = ':';
                m++;

                m += kit_string_percent_encode (buf + m, buf_size - m > 0 ? buf_size - m : 0, key);

                if (m < buf_size)
                        buf[m] = '=';
                m++;

                m += kit_string_percent_encode (buf + m, buf_size - m > 0 ? buf_size - m : 0, value);
        }

        if (m < buf_size)
                buf[m] = '\0';

out:
        return m;
}

static polkit_bool_t
_add_caller_constraints (char *buf, size_t buf_size, PolKitCaller *caller)
{
        PolKitAuthorizationConstraint *constraints[64];
        int num_constraints;
        polkit_bool_t ret;
        int num_written;
        int n;

        ret = FALSE;

        num_constraints = polkit_authorization_constraint_get_from_caller (caller, constraints, 64);
        if (num_constraints == -1)
                goto out;

        if (num_constraints >= 64) {
                goto out;
        }

        num_written = _write_constraints (buf, buf_size, constraints);
        if (num_written == -1) {
                goto out;
        }
        
        if ((size_t) num_written >= buf_size) {
                goto out;
        }
        
        ret = TRUE;

out:
        for (n = 0; n < num_constraints && n < 64 && constraints[n] != NULL; n++) {
                polkit_authorization_constraint_unref (constraints[n]);
        }
        return ret;
}

/**
 * polkit_authorization_db_add_entry_process_one_shot:
 * @authdb: the authorization database
 * @action: the action
 * @caller: the caller
 * @user_authenticated_as: the user that was authenticated
 *
 * Write an entry to the authorization database to indicate that the
 * given caller is authorized for the given action a single time.
 *
 * Note that this function should only be used by
 * <literal>libpolkit-grant</literal> or other sufficiently privileged
 * processes that deals with managing authorizations. It should never
 * be used by mechanisms or applications. The caller must have
 * egid=polkituser and umask set so creating files with mode 0460 will
 * work.
 *
 * This function is in <literal>libpolkit-grant</literal>.
 *
 * Returns: #TRUE if an entry was written to the authorization
 * database, #FALSE if the caller of this function is not sufficiently
 * privileged.
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_authorization_db_add_entry_process_one_shot (PolKitAuthorizationDB *authdb,
                                                    PolKitAction          *action,
                                                    PolKitCaller          *caller,
                                                    uid_t                  user_authenticated_as)
{
        char *action_id;
        uid_t caller_uid;
        pid_t caller_pid;
        polkit_bool_t ret;
        polkit_uint64_t pid_start_time;
        struct timeval now;

        g_return_val_if_fail (authdb != NULL, FALSE);
        g_return_val_if_fail (action != NULL, FALSE);
        g_return_val_if_fail (caller != NULL, FALSE);

        if (!polkit_action_get_action_id (action, &action_id))
                return FALSE;

        if (!polkit_caller_get_pid (caller, &caller_pid))
                return FALSE;

        if (!polkit_caller_get_uid (caller, &caller_uid))
                return FALSE;

        pid_start_time = polkit_sysdeps_get_start_time_for_pid (caller_pid);
        if (pid_start_time == 0)
                return FALSE;

        if (gettimeofday (&now, NULL) != 0) {
                g_warning ("Error calling gettimeofday: %m");
                return FALSE;
        }

        char pid_buf[32];
        char pid_st_buf[32];
        char now_buf[32];
        char uid_buf[32];
        char auth_buf[1024];
        snprintf (pid_buf, sizeof (pid_buf), "%d", caller_pid);
        snprintf (pid_st_buf, sizeof (pid_st_buf), "%Lu", pid_start_time);
        snprintf (now_buf, sizeof (now_buf), "%Lu", (polkit_uint64_t) now.tv_sec);
        snprintf (uid_buf, sizeof (uid_buf), "%d", user_authenticated_as);

        size_t len;
        if ((len = kit_string_entry_create (auth_buf, sizeof (auth_buf),
                                            "scope",          "process-one-shot",
                                            "pid",            pid_buf,
                                            "pid-start-time", pid_st_buf,
                                            "action-id",      action_id,
                                            "when",           now_buf,
                                            "auth-as",        uid_buf,
                                            NULL)) >= sizeof (auth_buf)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }

        if (!_add_caller_constraints (auth_buf + len, sizeof (auth_buf) - len, caller)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }

        ret = _polkit_authorization_db_auth_file_add (TRUE, 
                                                      caller_uid, 
                                                      auth_buf);
        return ret;
}

/**
 * polkit_authorization_db_add_entry_process:
 * @authdb: the authorization database
 * @action: the action
 * @caller: the caller
 * @user_authenticated_as: the user that was authenticated
 *
 * Write an entry to the authorization database to indicate that the
 * given caller is authorized for the given action.
 *
 * Note that this function should only be used by
 * <literal>libpolkit-grant</literal> or other sufficiently privileged
 * processes that deals with managing authorizations. It should never
 * be used by mechanisms or applications. The caller must have
 * egid=polkituser and umask set so creating files with mode 0460 will
 * work.
 *
 * This function is in <literal>libpolkit-grant</literal>.
 *
 * Returns: #TRUE if an entry was written to the authorization
 * database, #FALSE if the caller of this function is not sufficiently
 * privileged.
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_authorization_db_add_entry_process          (PolKitAuthorizationDB *authdb,
                                                    PolKitAction          *action,
                                                    PolKitCaller          *caller,
                                                    uid_t                  user_authenticated_as)
{
        char *action_id;
        uid_t caller_uid;
        pid_t caller_pid;
        polkit_bool_t ret;
        polkit_uint64_t pid_start_time;
        struct timeval now;

        g_return_val_if_fail (authdb != NULL, FALSE);
        g_return_val_if_fail (action != NULL, FALSE);
        g_return_val_if_fail (caller != NULL, FALSE);

        if (!polkit_action_get_action_id (action, &action_id))
                return FALSE;

        if (!polkit_caller_get_pid (caller, &caller_pid))
                return FALSE;

        if (!polkit_caller_get_uid (caller, &caller_uid))
                return FALSE;

        pid_start_time = polkit_sysdeps_get_start_time_for_pid (caller_pid);
        if (pid_start_time == 0)
                return FALSE;

        if (gettimeofday (&now, NULL) != 0) {
                g_warning ("Error calling gettimeofday: %m");
                return FALSE;
        }

        char pid_buf[32];
        char pid_st_buf[32];
        char now_buf[32];
        char uid_buf[32];
        char auth_buf[1024];
        snprintf (pid_buf, sizeof (pid_buf), "%d", caller_pid);
        snprintf (pid_st_buf, sizeof (pid_st_buf), "%Lu", pid_start_time);
        snprintf (now_buf, sizeof (now_buf), "%Lu", (polkit_uint64_t) now.tv_sec);
        snprintf (uid_buf, sizeof (uid_buf), "%d", user_authenticated_as);

        size_t len;
        if ((len = kit_string_entry_create (auth_buf, sizeof (auth_buf),
                                            "scope",          "process",
                                            "pid",            pid_buf,
                                            "pid-start-time", pid_st_buf,
                                            "action-id",      action_id,
                                            "when",           now_buf,
                                            "auth-as",        uid_buf,
                                            NULL)) >= sizeof (auth_buf)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }

        if (!_add_caller_constraints (auth_buf + len, sizeof (auth_buf) - len, caller)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }

        ret = _polkit_authorization_db_auth_file_add (TRUE, 
                                                      caller_uid, 
                                                      auth_buf);
        return ret;
}

/**
 * polkit_authorization_db_add_entry_session:
 * @authdb: the authorization database
 * @action: the action
 * @caller: the caller
 * @user_authenticated_as: the user that was authenticated
 *
 * Write an entry to the authorization database to indicate that the
 * session for the given caller is authorized for the given action for
 * the remainer of the session.
 *
 * Note that this function should only be used by
 * <literal>libpolkit-grant</literal> or other sufficiently privileged
 * processes that deals with managing authorizations. It should never
 * be used by mechanisms or applications. The caller must have
 * egid=polkituser and umask set so creating files with mode 0460 will
 * work.
 *
 * This function is in <literal>libpolkit-grant</literal>.
 *
 * Returns: #TRUE if an entry was written to the authorization
 * database, #FALSE if the caller of this function is not sufficiently
 * privileged.
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_authorization_db_add_entry_session          (PolKitAuthorizationDB *authdb,
                                                    PolKitAction          *action,
                                                    PolKitCaller          *caller,
                                                    uid_t                  user_authenticated_as)
{
        uid_t session_uid;
        char *action_id;
        PolKitSession *session;
        char *session_objpath;
        polkit_bool_t ret;
        struct timeval now;

        g_return_val_if_fail (authdb != NULL, FALSE);
        g_return_val_if_fail (action != NULL, FALSE);
        g_return_val_if_fail (caller != NULL, FALSE);

        if (!polkit_action_get_action_id (action, &action_id))
                return FALSE;

        if (!polkit_caller_get_ck_session (caller, &session))
                return FALSE;

        if (!polkit_session_get_ck_objref (session, &session_objpath))
                return FALSE;

        if (!polkit_session_get_uid (session, &session_uid))
                return FALSE;

        if (gettimeofday (&now, NULL) != 0) {
                g_warning ("Error calling gettimeofday: %m");
                return FALSE;
        }

        char now_buf[32];
        char uid_buf[32];
        char auth_buf[1024];
        snprintf (now_buf, sizeof (now_buf), "%Lu", (polkit_uint64_t) now.tv_sec);
        snprintf (uid_buf, sizeof (uid_buf), "%d", user_authenticated_as);

        size_t len;
        if ((len = kit_string_entry_create (auth_buf, sizeof (auth_buf),
                                            "scope",          "session",
                                            "session-id",     session_objpath,
                                            "action-id",      action_id,
                                            "when",           now_buf,
                                            "auth-as",        uid_buf,
                                            NULL)) >= sizeof (auth_buf)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }

        if (!_add_caller_constraints (auth_buf + len, sizeof (auth_buf) - len, caller)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }

        ret = _polkit_authorization_db_auth_file_add (TRUE, 
                                                      session_uid, 
                                                      auth_buf);
        return ret;
}

/**
 * polkit_authorization_db_add_entry_always:
 * @authdb: the authorization database
 * @action: the action
 * @caller: the caller
 * @user_authenticated_as: the user that was authenticated
 *
 * Write an entry to the authorization database to indicate that the
 * given user is authorized for the given action.
 *
 * Note that this function should only be used by
 * <literal>libpolkit-grant</literal> or other sufficiently privileged
 * processes that deals with managing authorizations. It should never
 * be used by mechanisms or applications. The caller must have
 * egid=polkituser and umask set so creating files with mode 0460 will
 * work.
 *
 * This function is in <literal>libpolkit-grant</literal>.
 *
 * Returns: #TRUE if an entry was written to the authorization
 * database, #FALSE if the caller of this function is not sufficiently
 * privileged.
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_authorization_db_add_entry_always           (PolKitAuthorizationDB *authdb,
                                                    PolKitAction          *action,
                                                    PolKitCaller          *caller,
                                                    uid_t                  user_authenticated_as)
{
        uid_t uid;
        char *action_id;
        polkit_bool_t ret;
        struct timeval now;

        g_return_val_if_fail (authdb != NULL, FALSE);
        g_return_val_if_fail (action != NULL, FALSE);
        g_return_val_if_fail (caller != NULL, FALSE);

        if (!polkit_caller_get_uid (caller, &uid))
                return FALSE;

        if (!polkit_action_get_action_id (action, &action_id))
                return FALSE;

        if (gettimeofday (&now, NULL) != 0) {
                g_warning ("Error calling gettimeofday: %m");
                return FALSE;
        }

        char now_buf[32];
        char uid_buf[32];
        char auth_buf[1024];
        snprintf (now_buf, sizeof (now_buf), "%Lu", (polkit_uint64_t) now.tv_sec);
        snprintf (uid_buf, sizeof (uid_buf), "%d", user_authenticated_as);

        size_t len;
        if ((len = kit_string_entry_create (auth_buf, sizeof (auth_buf),
                                            "scope",          "always",
                                            "action-id",      action_id,
                                            "when",           now_buf,
                                            "auth-as",        uid_buf,
                                            NULL)) >= sizeof (auth_buf)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }
        if (!_add_caller_constraints (auth_buf + len, sizeof (auth_buf) - len, caller)) {
                g_warning ("authbuf for is too small");
                return FALSE;
        }

        ret = _polkit_authorization_db_auth_file_add (FALSE, 
                                                      uid, 
                                                      auth_buf);
        return ret;
}


typedef struct {
        char *action_id;
        unsigned int _check_constraint_num;
        PolKitAuthorizationConstraint  **constraints;

        polkit_bool_t is_authorized;
        polkit_bool_t is_negative_authorized;
} CheckDataGrant;

static polkit_bool_t
_check_constraints_for_grant (PolKitAuthorization *auth, PolKitAuthorizationConstraint *authc, void *user_data)
{
        CheckDataGrant *cd = (CheckDataGrant *) user_data;
        polkit_bool_t ret;

        ret = FALSE;

        if (cd->constraints [cd->_check_constraint_num] == NULL)
                goto no_match;

        if (!polkit_authorization_constraint_equal (authc, cd->constraints[cd->_check_constraint_num]))
                goto no_match;

        cd->_check_constraint_num += 1;
        return FALSE;

no_match:
        return TRUE;
}

static polkit_bool_t 
_check_auth_for_grant (PolKitAuthorizationDB *authdb, PolKitAuthorization *auth, void *user_data)
{
        uid_t pimp;
        polkit_bool_t ret;
        polkit_bool_t is_negative;
        CheckDataGrant *cd = (CheckDataGrant *) user_data;

        ret = FALSE;

        if (strcmp (polkit_authorization_get_action_id (auth), cd->action_id) != 0)
                goto no_match;

        if (!polkit_authorization_was_granted_explicitly (auth, &pimp, &is_negative))
                goto no_match;

        /* This checks that the number of authorizations are the
         * same.. as well as that the authorizations are similar one
         * by one..
         *
         * TODO: FIXME: this relies on the ordering, e.g. we don't
         * catch local+active if there is an active+local one already.
         */
        cd->_check_constraint_num = 0;
        if (polkit_authorization_constraints_foreach (auth, _check_constraints_for_grant, cd) ||
            cd->constraints [cd->_check_constraint_num] != NULL)
                goto no_match;

        if (is_negative) {
                cd->is_authorized = FALSE;
                cd->is_negative_authorized = TRUE;
                /* it only takes a single negative auth to block things so stop iterating */
                ret = TRUE;
        } else {
                cd->is_authorized = TRUE;
                cd->is_negative_authorized = FALSE;
                /* keep iterating; we may find negative auths... */
        }

no_match:
        return ret;
}

static polkit_bool_t 
_grant_internal (PolKitAuthorizationDB          *authdb,
                 PolKitAction                   *action,
                 uid_t                           uid,
                 PolKitAuthorizationConstraint **constraints,
                 PolKitError                   **error,
                 polkit_bool_t                   is_negative)
{
        GError *g_error;
        char *helper_argv[6] = {PACKAGE_LIBEXEC_DIR "/polkit-explicit-grant-helper", NULL, NULL, NULL, NULL, NULL};
        gboolean ret;
        gint exit_status;
        char cbuf[1024];
        CheckDataGrant cd;
        polkit_bool_t did_exist;

        ret = FALSE;

        g_return_val_if_fail (authdb != NULL, FALSE);
        g_return_val_if_fail (action != NULL, FALSE);

        if (!polkit_action_get_action_id (action, &(cd.action_id))) {
                polkit_error_set_error (error, 
                                        POLKIT_ERROR_GENERAL_ERROR, 
                                        "Given action does not have action_id set");
                goto out;
        }

        if (constraints == NULL) {
                cbuf[0] = '\0';
        } else {
                int num_written;
                num_written = _write_constraints (cbuf, sizeof (cbuf), constraints);
                if (num_written == -1) {
                        polkit_error_set_error (error, 
                                                POLKIT_ERROR_GENERAL_ERROR, 
                                                "one of the given constraints did not fit");
                        goto out;
                }

                if ((size_t) num_written >= sizeof (cbuf)) {
                        g_warning ("buffer for auth constraint is too small");
                        polkit_error_set_error (error, 
                                                POLKIT_ERROR_GENERAL_ERROR, 
                                                "buffer for auth constraint is too small");
                        goto out;
                }
        }

        /* check if we have the auth already */
        cd.constraints = constraints;
        cd.is_authorized = FALSE;
        cd.is_negative_authorized = FALSE;
        polkit_authorization_db_foreach_for_uid (authdb,
                                                 uid, 
                                                 _check_auth_for_grant,
                                                 &cd,
                                                 error);

        /* happens if caller can't read auths of target user */
        if (error != NULL && polkit_error_is_set (*error)) {
                goto out;
        }

        did_exist = FALSE;
        if (is_negative) {
                if (cd.is_negative_authorized)
                        did_exist = TRUE;
        } else {
                if (cd.is_authorized)
                        did_exist = TRUE;
        }
        
        if (did_exist) {
                /* so it did exist.. */
                polkit_error_set_error (error, 
                                        POLKIT_ERROR_AUTHORIZATION_ALREADY_EXISTS, 
                                        "An authorization for uid %d for the action %s with constraint '%s' already exists",
                                        uid, cd.action_id, cbuf);
                goto out;
        }


        helper_argv[1] = cd.action_id;
        helper_argv[2] = cbuf;
        if (is_negative)
                helper_argv[3] = "uid-negative";
        else
                helper_argv[3] = "uid";
        helper_argv[4] = g_strdup_printf ("%d", uid);
        helper_argv[5] = NULL;

        g_error = NULL;
        if (!g_spawn_sync (NULL,         /* const gchar *working_directory */
                           helper_argv,  /* gchar **argv */
                           NULL,         /* gchar **envp */
                           0,            /* GSpawnFlags flags */
                           NULL,         /* GSpawnChildSetupFunc child_setup */
                           NULL,         /* gpointer user_data */
                           NULL,         /* gchar **standard_output */
                           NULL,         /* gchar **standard_error */
                           &exit_status, /* gint *exit_status */
                           &g_error)) {  /* GError **error */
                polkit_error_set_error (error, 
                                        POLKIT_ERROR_GENERAL_ERROR, 
                                        "Error spawning explicit grant helper: %s",
                                        g_error->message);
                g_error_free (g_error);
                goto out;
        }

        if (!WIFEXITED (exit_status)) {
                g_warning ("Explicit grant helper crashed!");
                polkit_error_set_error (error, 
                                        POLKIT_ERROR_GENERAL_ERROR, 
                                        "Explicit grant helper crashed!");
                goto out;
        } else if (WEXITSTATUS(exit_status) != 0) {
                polkit_error_set_error (error, 
                                        POLKIT_ERROR_NOT_AUTHORIZED_TO_GRANT_AUTHORIZATION, 
                                        "uid %d is not authorized to grant authorization for action %s to uid %d (requires org.freedesktop.policykit.grant)",
                                        getuid (), cd.action_id, uid);
        } else {
                ret = TRUE;
        }
        
out:
        g_free (helper_argv[4]);
        return ret;
}

/**
 * polkit_authorization_db_grant_to_uid:
 * @authdb: authorization database
 * @action: action
 * @uid: uid to grant to
 * @constraints: Either %NULL or a %NULL terminated list of constraint to put on the authorization
 * @error: return location for error
 *
 * Grants an authorization to a user for a specific action. This
 * requires the org.freedesktop.policykit.grant authorization.
 *
 * This function is in <literal>libpolkit-grant</literal>.
 *
 * Returns: #TRUE if the authorization was granted, #FALSE otherwise
 * and error will be set
 *
 * Since: 0.7
 */
polkit_bool_t 
polkit_authorization_db_grant_to_uid (PolKitAuthorizationDB          *authdb,
                                      PolKitAction                   *action,
                                      uid_t                           uid,
                                      PolKitAuthorizationConstraint **constraints,
                                      PolKitError                   **error)
{
        return _grant_internal (authdb, action, uid, constraints, error, FALSE);
}

/**
 * polkit_authorization_db_grant_negative_to_uid:
 * @authdb: authorization database
 * @action: action
 * @uid: uid to grant to
 * @constraints: Either %NULL or a %NULL terminated list of constraint to put on the authorization
 * @error: return location for error
 *
 * Grants a negative authorization to a user for a specific action. If
 * @uid differs from the calling user, the
 * org.freedesktop.policykit.grant authorization is required. In other
 * words, users may "grant" negative authorizations to themselves.
 *
 * A negative authorization is normally used to block users that would
 * normally be authorized from an implicit authorization.
 *
 * This function is in <literal>libpolkit-grant</literal>.
 *
 * Returns: #TRUE if the authorization was granted, #FALSE otherwise
 * and error will be set
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_authorization_db_grant_negative_to_uid           (PolKitAuthorizationDB          *authdb,
                                                         PolKitAction                   *action,
                                                         uid_t                           uid,
                                                         PolKitAuthorizationConstraint **constraints,
                                                         PolKitError                   **error)
{
        return _grant_internal (authdb, action, uid, constraints, error, TRUE);
}
