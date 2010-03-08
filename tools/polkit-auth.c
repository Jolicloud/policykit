/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-auth.c : grant privileges to a user through authentication
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
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#if defined(HAVE_SOLARIS) || defined(HAVE_FREEBSD)
#include <sys/wait.h>
#endif
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>

#include <polkit-dbus/polkit-dbus.h>
#include <polkit-grant/polkit-grant.h>

#include <glib.h>

static DBusConnection *system_bus;
static PolKitContext *pk_context;
static PolKitAuthorizationDB *pk_authdb;
static PolKitTracker *pk_tracker;
static PolKitCaller *pk_caller;

static gboolean opt_is_version;
static char *opt_obtain_action_id;
static polkit_bool_t opt_show_explicit;
static polkit_bool_t opt_show_explicit_detail;
static polkit_bool_t opt_show_obtainable;
static char *opt_revoke_action_id;
static char *opt_user;
static char *opt_grant_action_id;
static char *opt_block_action_id;

typedef struct {
        gboolean obtained_privilege;
        GMainLoop *loop;
} UserData;

#ifndef HAVE_GETLINE
static ssize_t
getline (char **lineptr, size_t *n, FILE *stream)
{
  char *line, *p;
  long size, copy;

  if (lineptr == NULL || n == NULL) {
          errno = EINVAL;
          return (ssize_t) -1;
  }

  if (ferror (stream))
          return (ssize_t) -1;

  /* Make sure we have a line buffer to start with.  */
  if (*lineptr == NULL || *n < 2) /* !seen and no buf yet need 2 chars.  */ {
#ifndef        MAX_CANON
#define        MAX_CANON        256
#endif
          if (!*lineptr)
                  line = (char *) malloc (MAX_CANON);
          else
                  line = (char *) realloc (*lineptr, MAX_CANON);
          if (line == NULL)
                  return (ssize_t) -1;
          *lineptr = line;
          *n = MAX_CANON;
  }

  line = *lineptr;
  size = *n;

  copy = size;
  p = line;

  while (1) {
          long len;

          while (--copy > 0) {
                  int c = getc (stream);

                  if (c == EOF)
                          goto lose;
                  else if ((*p++ = c) == '\n')
                          goto win;
          }

          /* Need to enlarge the line buffer.  */
          len = p - line;
          size *= 2;
          line = (char *) realloc (line, size);
          if (line == NULL)
                  goto lose;
          *lineptr = line;
          *n = size;
          p = line + len;
          copy = size - len;
  }

lose:
  if (p == *lineptr)
          return (ssize_t) -1;

  /* Return a partial line since we got an error in the middle.  */
win:
  *p = '\0';
  return p - *lineptr;
}
#endif

static void
conversation_type (PolKitGrant *polkit_grant, PolKitResult auth_type, void *user_data)
{
        switch (auth_type) {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
                printf ("Authentication as an administrative user is required.\n");
                break;

        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
                printf ("Authentication is required.\n");
                break;

        default:
                /* should never happen */
                exit (1);
        }
}

static char *
conversation_select_admin_user (PolKitGrant *polkit_grant, char **admin_users, void *user_data)
{
        int n;
        char *user;
        char *lineptr = NULL;
        size_t linelen = 0;

        printf ("The following users qualify as administrative users: ");
        for (n = 0; admin_users[n] != NULL; n++) {
                printf ("%s ", admin_users[n]);
        }
        printf ("\n");
        printf ("Select user: ");
        getline (&lineptr, &linelen, stdin);
        user = strdup (lineptr);
        free (lineptr);
        return user;
}

static char *
conversation_pam_prompt_echo_off (PolKitGrant *polkit_grant, const char *request, void *user_data)
{
        char *lineptr = NULL;
        size_t linelen = 0;
        struct termios old, new;
        char *result;

        printf ("%s", request);

        /* Turn echo off */
        if (tcgetattr (fileno (stdout), &old) != 0) {
                exit (1);
        }
        new = old;
        new.c_lflag &= ~ECHO;
        if (tcsetattr (fileno (stdout), TCSAFLUSH, &new) != 0) {
                exit (1);
        }

        getline (&lineptr, &linelen, stdin);
  
        /* Restore terminal. */
        tcsetattr (fileno (stdout), TCSAFLUSH, &old);

        result = strdup (lineptr);
        free (lineptr);
        printf ("\n");
        return result;
}

static char *
conversation_pam_prompt_echo_on (PolKitGrant *polkit_grant, const char *request, void *user_data)
{
        char *lineptr = NULL;
        size_t linelen = 0;
        char *result;
        printf ("%s", request);
        getline (&lineptr, &linelen, stdin);
        result = strdup (lineptr);
        free (lineptr);
        printf ("\n");
        return result;
}

static void
conversation_pam_error_msg (PolKitGrant *polkit_grant, const char *msg, void *user_data)
{
        printf ("Error from PAM: %s\n", msg);
}

static void
conversation_pam_text_info (PolKitGrant *polkit_grant, const char *msg, void *user_data)
{
        printf ("Info from PAM: %s\n", msg);
}

static PolKitResult
conversation_override_grant_type (PolKitGrant *polkit_grant, PolKitResult auth_type, void *user_data)
{
        char *lineptr = NULL;
        size_t linelen = 0;
        polkit_bool_t keep_session = FALSE;
        polkit_bool_t keep_always = FALSE;
        PolKitResult overridden_auth_type;

        switch (auth_type) {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
                break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
                printf ("Keep this privilege for the session? [no/session]?\n");
        again:
                getline (&lineptr, &linelen, stdin);
                if (g_str_has_prefix (lineptr, "no")) {
                        ;
                } else if (g_str_has_prefix (lineptr, "session")) {
                        keep_session = TRUE;
                } else {
                        printf ("Valid responses are 'no' and 'session'. Try again.\n");
                        goto again;
                }
                free (lineptr);
                break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
                printf ("Keep this privilege for the session or always? [no/session/always]?\n");
        again2:
                getline (&lineptr, &linelen, stdin);
                if (g_str_has_prefix (lineptr, "no")) {
                        ;
                } else if (g_str_has_prefix (lineptr, "session")) {
                        keep_session = TRUE;
                } else if (g_str_has_prefix (lineptr, "always")) {
                        keep_always = TRUE;
                } else {
                        printf ("Valid responses are 'no', 'session' and 'always'. Try again.\n");
                        goto again2;
                }
                free (lineptr);
                break;
        default:
                /* should never happen */
                exit (1);
        }

        switch (auth_type) {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
                overridden_auth_type = POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH;
                if (keep_session)
                        overridden_auth_type = POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION;
                else if (keep_always)
                        overridden_auth_type = POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS;
                break;

        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
                overridden_auth_type = POLKIT_RESULT_ONLY_VIA_SELF_AUTH;
                if (keep_session)
                        overridden_auth_type = POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION;
                else if (keep_always)
                        overridden_auth_type = POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS;
                break;

        default:
                /* should never happen */
                exit (1);
        }

        return overridden_auth_type;
}

static void 
conversation_done (PolKitGrant *polkit_grant, 
                   polkit_bool_t obtained_privilege, 
                   polkit_bool_t invalid_data, 
                   void *user_data)
{
        UserData *ud = user_data;
        ud->obtained_privilege = obtained_privilege;
        g_main_loop_quit (ud->loop);
}




static void
child_watch_func (GPid pid,
                  gint status,
                  gpointer user_data)
{
        PolKitGrant *polkit_grant = user_data;
        polkit_grant_child_func (polkit_grant, pid, WEXITSTATUS (status));
}

static int
add_child_watch (PolKitGrant *polkit_grant, pid_t pid)
{
        return g_child_watch_add (pid, child_watch_func, polkit_grant);
}

static gboolean
io_watch_have_data (GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
        int fd;
        PolKitGrant *polkit_grant = user_data;
        fd = g_io_channel_unix_get_fd (channel);
        polkit_grant_io_func (polkit_grant, fd);
        return TRUE;
}

static int
add_io_watch (PolKitGrant *polkit_grant, int fd)
{
        guint id = 0;
        GIOChannel *channel;
        channel = g_io_channel_unix_new (fd);
        if (channel == NULL)
                goto out;
        id = g_io_add_watch (channel, G_IO_IN, io_watch_have_data, polkit_grant);
        if (id == 0) {
                g_io_channel_unref (channel);
                goto out;
        }
        g_io_channel_unref (channel);
out:
        return id;
}

static void 
remove_watch (PolKitGrant *polkit_auth, int watch_id)
{
        g_source_remove (watch_id);
}

static polkit_bool_t
obtain_authorization (const char *action_id)
{
        UserData ud;
        PolKitAction *action;
        PolKitGrant *polkit_grant;

        /* TODO: Attempt to use a service like PolicyKit-gnome on the session bus if available.. */

        printf ("Attempting to obtain authorization for %s.\n", action_id);
        
        ud.loop = g_main_loop_new (NULL, TRUE);
        ud.obtained_privilege = FALSE;

        action = polkit_action_new ();
        polkit_action_set_action_id (action, action_id);
        
        
        polkit_grant = polkit_grant_new ();
        if (polkit_grant == NULL) {
                fprintf (stderr, "polkit-auth: authorization database does not support this operation.\n");
                goto out;
        }

        polkit_grant_set_functions (polkit_grant,
                                    add_io_watch,
                                    add_child_watch,
                                    remove_watch,
                                    conversation_type,
                                    conversation_select_admin_user,
                                    conversation_pam_prompt_echo_off,
                                    conversation_pam_prompt_echo_on,
                                    conversation_pam_error_msg,
                                    conversation_pam_text_info,
                                    conversation_override_grant_type,
                                    conversation_done,
                                    &ud);
        
        if (!polkit_grant_initiate_auth (polkit_grant,
                                         action,
                                         pk_caller)) {
                fprintf (stderr, "polkit-auth: failed to initiate privilege grant.\n");
                goto out;
        }
        g_main_loop_run (ud.loop);
        polkit_grant_unref (polkit_grant);

        if (ud.obtained_privilege)
                printf ("Successfully obtained the authorization for %s.\n", action_id);
        else
                printf ("Failed to obtain authorization for %s.\n", action_id);

out:
        return ud.obtained_privilege;
}

static const char *
get_name_from_uid (uid_t uid)
{
        const char *name;
        struct passwd *pw;

        pw = getpwuid (uid);
        if (pw != NULL)
                name = (const char *) pw->pw_name;
        else
                name = "(unknown)";

        return name;
}

static polkit_bool_t 
_print_constraint (PolKitAuthorization *auth, PolKitAuthorizationConstraint *authc, void *user_data)
{
        switch (polkit_authorization_constraint_type (authc)) {
        case POLKIT_AUTHORIZATION_CONSTRAINT_TYPE_REQUIRE_LOCAL:
                printf ("  Constraint:  Session must be on a local console\n");
                break;
        case POLKIT_AUTHORIZATION_CONSTRAINT_TYPE_REQUIRE_ACTIVE:
                printf ("  Constraint:  Session must be active\n");
                break;
        case POLKIT_AUTHORIZATION_CONSTRAINT_TYPE_REQUIRE_EXE:
                printf ("  Constraint:  Only allowed for program %s\n", 
                        polkit_authorization_constraint_get_exe (authc));
                break;
        case POLKIT_AUTHORIZATION_CONSTRAINT_TYPE_REQUIRE_SELINUX_CONTEXT:
                printf ("  Constraint:  Only allowed for SELinux Context %s\n", 
                        polkit_authorization_constraint_get_selinux_context (authc));
                break;
        }
        return FALSE;
}

static polkit_bool_t
auth_iterator_cb (PolKitAuthorizationDB *authdb,
                  PolKitAuthorization   *auth, 
                  void                  *user_data)
{
        const char *action_id;
        DBusError dbus_error;
        GHashTable *already_shown = (GHashTable *) user_data;

        action_id = polkit_authorization_get_action_id (auth);

        if (!opt_show_explicit_detail) {
                if (g_hash_table_lookup (already_shown, action_id) != NULL)
                        goto out;
        }

        dbus_error_init (&dbus_error);
        if (!polkit_tracker_is_authorization_relevant (pk_tracker, auth, &dbus_error)) {
                if (dbus_error_is_set (&dbus_error)) {
                        g_warning ("Cannot determine if authorization is relevant: %s: %s",
                                   dbus_error.name,
                                   dbus_error.message);
                        dbus_error_free (&dbus_error);
                } else {
                        goto out;
                }
        }

        if (!opt_show_explicit_detail) {
                g_hash_table_insert (already_shown, g_strdup (action_id), (gpointer) 1);
        }

        if (opt_show_explicit_detail) {
                char *s;
                time_t time_granted;
                struct tm *time_tm;
                char time_string[128];
                uid_t auth_uid;
                uid_t pimp_uid;
                polkit_bool_t is_negative;
                pid_t pid;
                polkit_uint64_t pid_start_time;
                PolKitAction *pk_action;
                PolKitResult pk_result;
                char exe[PATH_MAX];

                printf ("%s\n", action_id);

                pk_action = polkit_action_new ();
                polkit_action_set_action_id (pk_action, action_id);
                pk_result = polkit_context_is_caller_authorized (pk_context, pk_action, pk_caller, FALSE, NULL);
                polkit_action_unref (pk_action);
                printf ("  Authorized:  %s\n", pk_result == POLKIT_RESULT_YES ? "Yes" : "No");

                switch (polkit_authorization_get_scope (auth)) {
                case POLKIT_AUTHORIZATION_SCOPE_PROCESS_ONE_SHOT:
                case POLKIT_AUTHORIZATION_SCOPE_PROCESS:
                        polkit_authorization_scope_process_get_pid (auth, &pid, &pid_start_time);
                        if (polkit_sysdeps_get_exe_for_pid (pid, exe, sizeof (exe)) == -1)
                                strncpy (exe, "unknown", sizeof (exe));

                        if (polkit_authorization_get_scope (auth) == POLKIT_AUTHORIZATION_SCOPE_PROCESS_ONE_SHOT) {
                                printf ("  Scope:       Confined to single shot from pid %d (%s)\n", pid, exe);
                        } else {
                                printf ("  Scope:       Confined to pid %d (%s)\n", pid, exe);
                        }
                        break;
                case POLKIT_AUTHORIZATION_SCOPE_SESSION:
                        printf ("  Scope:       Confined to session %s\n", polkit_authorization_scope_session_get_ck_objref (auth));
                        break;
                case POLKIT_AUTHORIZATION_SCOPE_ALWAYS:
                        printf ("  Scope:       Indefinitely\n");
                        break;
                }

                time_granted = polkit_authorization_get_time_of_grant (auth);
                time_tm = localtime (&time_granted);

                is_negative = FALSE;
                if (polkit_authorization_was_granted_via_defaults (auth, &auth_uid)) { 
                        s = g_strdup_printf ("%%c by auth as %s (uid %d)", get_name_from_uid (auth_uid), auth_uid);
                        strftime (time_string, sizeof (time_string), s, time_tm);
                        g_free (s);
                } else if (polkit_authorization_was_granted_explicitly (auth, &pimp_uid, &is_negative)) { 
                        s = g_strdup_printf ("%%c from %s (uid %d)", get_name_from_uid (pimp_uid), pimp_uid);
                        strftime (time_string, sizeof (time_string), s, time_tm);
                        g_free (s);
                } else {
                        strftime (time_string, sizeof (time_string), "%c", time_tm);
                }
                printf ("  Obtained:    %s\n", time_string);

                polkit_authorization_constraints_foreach (auth, _print_constraint, NULL);

                if (is_negative) {
                        printf ("  Negative:    Yes\n");
                }

                printf ("\n");
        } else {
                uid_t pimp_uid;
                polkit_bool_t is_negative;

                if (!polkit_authorization_was_granted_explicitly (auth, &pimp_uid, &is_negative))
                        is_negative = FALSE;

                if (!is_negative)
                        printf ("%s\n", action_id);
        }


out:
        return FALSE;
}

static polkit_bool_t
pfe_iterator_cb (PolKitPolicyCache *policy_cache,
                 PolKitPolicyFileEntry *pfe,
                 void *user_data)
{
        PolKitAction *action;

        action = polkit_action_new ();
        polkit_action_set_action_id (action, polkit_policy_file_entry_get_id (pfe));

        if (polkit_context_is_caller_authorized (pk_context,
                                                 action,
                                                 pk_caller,
                                                 FALSE,
                                                 NULL) == POLKIT_RESULT_YES) {
                printf ("%s\n", polkit_policy_file_entry_get_id (pfe));
        }

        polkit_action_unref (action);

        return FALSE;
}

static polkit_bool_t
pfe_iterator_show_obtainable_cb (PolKitPolicyCache *policy_cache,
                                 PolKitPolicyFileEntry *pfe,
                                 void *user_data)
{
        PolKitAction *action;

        action = polkit_action_new ();
        polkit_action_set_action_id (action, polkit_policy_file_entry_get_id (pfe));

        switch (polkit_context_is_caller_authorized (pk_context,
                                                     action,
                                                     pk_caller,
                                                     FALSE,
                                                     NULL)) {
        default:
        case POLKIT_RESULT_UNKNOWN:
        case POLKIT_RESULT_NO:
        case POLKIT_RESULT_YES:
                break;

        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
                printf ("%s\n", polkit_policy_file_entry_get_id (pfe));
                break;
        }

        polkit_action_unref (action);

        return FALSE;
}


static polkit_bool_t
auth_revoke_iterator_cb (PolKitAuthorizationDB *authdb,
                         PolKitAuthorization   *auth, 
                         void                  *user_data)
{
        PolKitError *pk_error;

        pk_error = NULL;
        if (!polkit_authorization_db_revoke_entry (authdb, auth, &pk_error)) {
                fprintf (stderr, "polkit-auth: %s: %s\n", 
                         polkit_error_get_error_name (pk_error),
                         polkit_error_get_error_message (pk_error));
                polkit_error_free (pk_error);
        }

        return FALSE;
}

static polkit_bool_t
revoke_authorizations (const char *action_id, uid_t uid)
{
        PolKitAction *pk_action;
        PolKitError *pk_error;
        polkit_bool_t ret;

        ret = FALSE;

        pk_action = polkit_action_new ();
        polkit_action_set_action_id (pk_action, action_id);

        pk_error = NULL;
        if (!polkit_authorization_db_foreach_for_action_for_uid (pk_authdb,
                                                                 pk_action,
                                                                 uid,
                                                                 auth_revoke_iterator_cb,
                                                                 NULL,
                                                                 &pk_error)) {
                if (polkit_error_is_set (pk_error)) {
                        fprintf (stderr, "polkit-auth: %s\n",
                                 polkit_error_get_error_message (pk_error));
                        polkit_error_free (pk_error);
                        goto out;
                }
        }

        ret = TRUE;
out:
        return ret;
}

static void
usage (int argc, char *argv[])
{
        execlp ("man", "man", "polkit-auth", NULL);
        fprintf (stderr, "Cannot show man page: %m\n");
        exit (1);
}

static polkit_bool_t
ensure_dbus_and_ck (void)
{
        if (pk_caller != NULL)
                return TRUE;

        fprintf (stderr, "polkit-auth: This operation requires the system message bus and ConsoleKit to be running\n");

        return FALSE;
}

#define MAX_CONSTRAINTS 64

int
main (int argc, char *argv[])
{
        int ret;
        PolKitError *pk_error;
	DBusError dbus_error;
        struct passwd *pw;
        uid_t uid;
        pid_t pid;
        char *s;
        PolKitAuthorizationConstraint *constraints[MAX_CONSTRAINTS];
        unsigned int num_constraints = 0;

        ret = 1;

        pk_error = NULL;
        pk_context = polkit_context_new ();
        if (!polkit_context_init (pk_context, &pk_error)) {
                fprintf (stderr, "polkit-auth: %s: %s\n", 
                         polkit_error_get_error_name (pk_error),
                         polkit_error_get_error_message (pk_error));
                polkit_error_free (pk_error);
                goto out;
        }

        pk_authdb = polkit_context_get_authorization_db (pk_context);

        /* Since polkit-auth will be used in e.g. RPM's %post (for example to grant 
         * org.freedesktop.policykit.read to services dropping privileges (like hald)) 
         * we need to be able to run even when D-Bus and/or ConsoleKit aren't available...
         */

        if ((s = getenv ("POLKIT_AUTH_GRANT_TO_PID")) != NULL) {
                pid = atoi (s);
        } else {
                pid = getppid ();
        }

        dbus_error_init (&dbus_error);
        system_bus = dbus_bus_get (DBUS_BUS_SYSTEM, &dbus_error);
        if (system_bus != NULL) {
                pk_tracker = polkit_tracker_new ();
                polkit_tracker_set_system_bus_connection (pk_tracker, system_bus);
                polkit_tracker_init (pk_tracker);
                
                pk_caller = polkit_caller_new_from_pid (system_bus, pid, &dbus_error);
                if (pk_caller == NULL) {
                        if (dbus_error_is_set (&dbus_error)) {
                                fprintf (stderr, "polkit-auth: polkit_caller_new_from_dbus_name(): %s: %s\n", 
                                         dbus_error.name, dbus_error.message);
                                goto out;
                        }
                }
        } else {
                pk_tracker = NULL;
                pk_caller = NULL;
        }

        opt_show_explicit = FALSE;
        opt_show_explicit_detail = FALSE;
        opt_is_version = FALSE;
        opt_obtain_action_id = NULL;
        opt_grant_action_id = NULL;
        opt_block_action_id = NULL;
        opt_revoke_action_id = NULL;
        opt_show_obtainable = FALSE;
        opt_user = NULL;

	while (argc > 1) {
		int c;
		int option_index = 0;
		const char *opt;
		static struct option long_options[] = {
                        {"explicit", 0, NULL, 0},
                        {"explicit-detail", 0, NULL, 0},
			{"obtain", 1, NULL, 0},
			{"grant", 1, NULL, 0},
			{"block", 1, NULL, 0},
                        {"constraint", 1, NULL, 0},
			{"revoke", 1, NULL, 0},
			{"show-obtainable", 0, NULL, 0},
			{"user", 1, NULL, 0},
			{"version", 0, NULL, 0},
			{"help", 0, NULL, 0},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long (argc, argv, "", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			opt = long_options[option_index].name;

			if (strcmp (opt, "help") == 0) {
				usage (argc, argv);
				return 0;
			} else if (strcmp (opt, "version") == 0) {
				opt_is_version = TRUE;
			} else if (strcmp (opt, "obtain") == 0) {
				opt_obtain_action_id = strdup (optarg);
			} else if (strcmp (opt, "grant") == 0) {
				opt_grant_action_id = strdup (optarg);
			} else if (strcmp (opt, "block") == 0) {
				opt_block_action_id = strdup (optarg);
			} else if (strcmp (opt, "constraint") == 0) {
                                PolKitAuthorizationConstraint *c;

                                c = polkit_authorization_constraint_from_string (optarg);
                                if (c == NULL) {
                                        fprintf (stderr, "polkit-auth: constraint '%s' not recognized\n", optarg);
                                        goto out;
                                }

                                if (num_constraints >= MAX_CONSTRAINTS - 1) {
                                        fprintf (stderr, "polkit-auth: Too many constraints specified\n");
                                        goto out;
                                }
                                constraints[num_constraints++] = c;

			} else if (strcmp (opt, "revoke") == 0) {
				opt_revoke_action_id = strdup (optarg);
			} else if (strcmp (opt, "show-obtainable") == 0) {
                                opt_show_obtainable = TRUE;
			} else if (strcmp (opt, "explicit") == 0) {
                                opt_show_explicit = TRUE;
			} else if (strcmp (opt, "explicit-detail") == 0) {
                                opt_show_explicit_detail = TRUE;
			} else if (strcmp (opt, "user") == 0) {
                                opt_user = strdup (optarg);
			}
			break;
                case '?':
                        usage (argc, argv);
                        goto out;
		}
	}

	if (opt_is_version) {
		printf ("polkit-auth " PACKAGE_VERSION "\n");
                ret = 0;
                goto out;
	}

        if (opt_user != NULL) {
                pw = getpwnam (opt_user);
                if (pw == NULL) {
                        fprintf (stderr, "polkit-auth: cannot look up uid for user '%s'\n", opt_user);
                        goto out;
                }
                uid = pw->pw_uid;
        } else {
                uid = getuid ();
        }

        if (opt_obtain_action_id != NULL) {
                if (!ensure_dbus_and_ck ())
                        goto out;

                if (getenv ("POLKIT_AUTH_FORCE_TEXT") != NULL) {
                        if (!obtain_authorization (opt_obtain_action_id))
                                goto out;                
                } else {
                        DBusError dbus_error;

                        dbus_error_init (&dbus_error);
                        if (!polkit_auth_obtain (opt_obtain_action_id, 0, pid, &dbus_error)) {
                                if (dbus_error_is_set (&dbus_error)) {
                                        
                                        /* fall back to text mode */
                                        if (!obtain_authorization (opt_obtain_action_id))
                                                goto out;
                                        
                                        //fprintf (stderr, 
                                        //         "polkit-auth: failed to use session service: %s: %s\n", 
                                        //         dbus_error.name, dbus_error.message);
                                } else {
                                        goto out;
                                }
                        }
                }

                ret = 0;
        } else if (opt_grant_action_id != NULL ||
                   opt_block_action_id != NULL) {
                PolKitAction *pk_action;
                PolKitError *pk_error;
                polkit_bool_t res;

                if (opt_user == NULL && uid == 0) {
                        fprintf (stderr, "polkit-auth: Cowardly refusing to grant authorization to uid 0 (did you forget to specify what user to grant to?). To force, run with --user root.\n");
                        goto out;
                }

                pk_action = polkit_action_new ();
                if (opt_block_action_id != NULL)
                        polkit_action_set_action_id (pk_action, opt_block_action_id);
                else
                        polkit_action_set_action_id (pk_action, opt_grant_action_id);

                constraints[num_constraints] = NULL;

                pk_error = NULL;
                if (opt_block_action_id != NULL) {
                        res = polkit_authorization_db_grant_negative_to_uid (pk_authdb,
                                                                             pk_action,
                                                                             uid,
                                                                             constraints,
                                                                             &pk_error);
                } else {
                        res = polkit_authorization_db_grant_to_uid (pk_authdb,
                                                                    pk_action,
                                                                    uid,
                                                                    constraints,
                                                                    &pk_error);
                }

                if (!res) {
                        fprintf (stderr, "polkit-auth: %s: %s\n", 
                                 polkit_error_get_error_name (pk_error),
                                 polkit_error_get_error_message (pk_error));
                        polkit_error_free (pk_error);
                        goto out;
                }
                
                ret = 0;

        } else if (opt_revoke_action_id != NULL) {
                if (!revoke_authorizations (opt_revoke_action_id, uid))
                        goto out;                
                ret = 0;
        } else if (opt_show_explicit || opt_show_explicit_detail) {
                GHashTable *already_shown;

                if (!ensure_dbus_and_ck ())
                        goto out;

                already_shown = g_hash_table_new_full (g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       NULL);


                /* first the explicit authorizations */
                pk_error = NULL;
                if (!polkit_authorization_db_foreach_for_uid (pk_authdb,
                                                              uid,
                                                              auth_iterator_cb,
                                                              already_shown,
                                                              &pk_error)) {
                        if (polkit_error_is_set (pk_error)) {
                                fprintf (stderr, "polkit-auth: %s: %s\n", 
                                         polkit_error_get_error_name (pk_error),
                                         polkit_error_get_error_message (pk_error));
                                polkit_error_free (pk_error);
                                goto out;
                        }                        
                }

                g_hash_table_destroy (already_shown);

                ret = 0;
        } else if (opt_show_obtainable) {
                PolKitPolicyCache *pk_policy_cache;

                if (!ensure_dbus_and_ck ())
                        goto out;

                /* show all authorizations; we do this by iterating over all actions and 
                 * then querying whether the caller is authorized 
                 */

                pk_policy_cache = polkit_context_get_policy_cache (pk_context);
                polkit_policy_cache_foreach (pk_policy_cache,
                                             pfe_iterator_show_obtainable_cb,
                                             NULL);
                ret = 0;
        } else {
                PolKitPolicyCache *pk_policy_cache;

                if (!ensure_dbus_and_ck ())
                        goto out;

                /* show all authorizations; we do this by iterating over all actions and 
                 * then querying whether the caller is authorized 
                 */

                pk_policy_cache = polkit_context_get_policy_cache (pk_context);
                polkit_policy_cache_foreach (pk_policy_cache,
                                             pfe_iterator_cb,
                                             NULL);
                ret = 0;
        }

out:
        return ret;
}
