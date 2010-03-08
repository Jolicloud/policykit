/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-caller.c : callers
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

/**
 * SECTION:polkit-caller
 * @title: Caller
 * @short_description: Represents a process requesting a mechanism to do something.
 *
 * This class is used to represent a caller in another process that is
 * calling into a mechanism to make the mechanism do something.
 **/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <errno.h>

#include "polkit-debug.h"
#include "polkit-caller.h"
#include "polkit-utils.h"
#include "polkit-test.h"
#include "polkit-private.h"

/**
 * PolKitCaller:
 *
 * Objects of this class are used to record information about a caller
 * in another process.
 **/
struct _PolKitCaller
{
        int refcount;
        char *dbus_name;
        uid_t uid;
        pid_t pid;
        char *selinux_context;
        PolKitSession *session;
};

/**
 * polkit_caller_new:
 *
 * Creates a new #PolKitCaller object.
 *
 * Returns: the new object
 **/
PolKitCaller *
polkit_caller_new (void)
{
        PolKitCaller *caller;
        caller = kit_new0 (PolKitCaller, 1);
        if (caller == NULL)
                goto out;
        caller->refcount = 1;
out:
        return caller;
}

/**
 * polkit_caller_ref:
 * @caller: The caller object
 * 
 * Increase reference count.
 * 
 * Returns: the object
 **/
PolKitCaller *
polkit_caller_ref (PolKitCaller *caller)
{
        kit_return_val_if_fail (caller != NULL, caller);
        caller->refcount++;
        return caller;
}


/**
 * polkit_caller_unref:
 * @caller: The caller object
 * 
 * Decreases the reference count of the object. If it becomes zero,
 * the object is freed. Before freeing, reference counts on embedded
 * objects are decresed by one.
 **/
void
polkit_caller_unref (PolKitCaller *caller)
{
        kit_return_if_fail (caller != NULL);
        caller->refcount--;
        if (caller->refcount > 0) 
                return;
        kit_free (caller->dbus_name);
        kit_free (caller->selinux_context);
        if (caller->session != NULL)
                polkit_session_unref (caller->session);
        kit_free (caller);
}

/**
 * polkit_caller_set_dbus_name:
 * @caller: The caller object
 * @dbus_name: unique system bus connection name
 * 
 * Set the callers unique system bus connection name.
 *
 * Returns: #TRUE only if the value validated and was set
 **/
polkit_bool_t
polkit_caller_set_dbus_name (PolKitCaller *caller, const char *dbus_name)
{
        kit_return_val_if_fail (caller != NULL, FALSE);

        if (dbus_name != NULL && ! _pk_validate_unique_bus_name (dbus_name))
                return FALSE;

        if (caller->dbus_name != NULL)
                kit_free (caller->dbus_name);
        if (dbus_name == NULL) {
                caller->dbus_name = NULL;
                return TRUE;
        } else {
                caller->dbus_name = kit_strdup (dbus_name);
                if (caller->dbus_name == NULL)
                        return FALSE;
                else
                        return TRUE;
        }
}

/**
 * polkit_caller_set_uid:
 * @caller: The caller object 
 * @uid: UNIX user id
 * 
 * Set the callers UNIX user id.
 *
 * Returns: #TRUE only if the value validated and was set
 **/
polkit_bool_t
polkit_caller_set_uid (PolKitCaller *caller, uid_t uid)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        caller->uid = uid;
        return TRUE;
}

/**
 * polkit_caller_set_pid:
 * @caller: The caller object 
 * @pid: UNIX process id
 * 
 * Set the callers UNIX process id.
 *
 * Returns: #TRUE only if the value validated and was set
 **/
polkit_bool_t
polkit_caller_set_pid (PolKitCaller *caller, pid_t pid)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        caller->pid = pid;
        return TRUE;
}

/**
 * polkit_caller_set_selinux_context:
 * @caller: The caller object 
 * @selinux_context: SELinux security context
 * 
 * Set the callers SELinux security context.
 *
 * Returns: #TRUE only if the value validated and was set
 **/
polkit_bool_t
polkit_caller_set_selinux_context (PolKitCaller *caller, const char *selinux_context)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        /* TODO: probably should have a separate validation function for SELinux contexts */
        kit_return_val_if_fail (selinux_context == NULL || _pk_validate_identifier (selinux_context), FALSE);

        if (caller->selinux_context != NULL)
                kit_free (caller->selinux_context);
        if (selinux_context == NULL) {
                caller->selinux_context = NULL;
                return TRUE;
        } else {
                caller->selinux_context = kit_strdup (selinux_context);
                if (caller->selinux_context == NULL)
                        return FALSE;
                else
                        return TRUE;
        }
}

/**
 * polkit_caller_set_ck_session:
 * @caller: The caller object 
 * @session: a session object
 * 
 * Set the callers session. The reference count on the given object
 * will be increased by one. If an existing session object was set
 * already, the reference count on that one will be decreased by one.
 *
 * Returns: #TRUE only if the value validated and was set
 **/
polkit_bool_t
polkit_caller_set_ck_session (PolKitCaller *caller, PolKitSession *session)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        kit_return_val_if_fail (session == NULL || polkit_session_validate (session), FALSE);
        if (caller->session != NULL)
                polkit_session_unref (caller->session);
        caller->session = session != NULL ? polkit_session_ref (session) : NULL;
        return TRUE;
}

/**
 * polkit_caller_get_dbus_name:
 * @caller: The caller object 
 * @out_dbus_name: Returns the unique system bus connection name. The caller shall not free this string.
 * 
 * Get the callers unique system bus connection name.
 * 
 * Returns: TRUE iff the value is returned
 **/
polkit_bool_t
polkit_caller_get_dbus_name (PolKitCaller *caller, char **out_dbus_name)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        kit_return_val_if_fail (out_dbus_name != NULL, FALSE);
        *out_dbus_name = caller->dbus_name;
        return TRUE;
}

/**
 * polkit_caller_get_uid:
 * @caller: The caller object 
 * @out_uid: Returns the UNIX user id
 * 
 * Get the callers UNIX user id.
 * 
 * Returns: TRUE iff the value is returned
 **/
polkit_bool_t
polkit_caller_get_uid (PolKitCaller *caller, uid_t *out_uid)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        kit_return_val_if_fail (out_uid != NULL, FALSE);
        *out_uid = caller->uid;
        return TRUE;
}

/**
 * polkit_caller_get_pid:
 * @caller: The caller object 
 * @out_pid: Returns the UNIX process id
 * 
 * Get the callers UNIX process id.
 * 
 * Returns: TRUE iff the value is returned
 **/
polkit_bool_t
polkit_caller_get_pid (PolKitCaller *caller, pid_t *out_pid)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        kit_return_val_if_fail (out_pid != NULL, FALSE);
        *out_pid = caller->pid;
        return TRUE;
}

/**
 * polkit_caller_get_selinux_context:
 * @caller: The caller object 
 * @out_selinux_context: Returns the SELinux security context. The caller shall not free this string.
 * 
 * Get the callers SELinux security context. Note that this may be
 * #NULL if SELinux is not available on the system.
 * 
 * Returns: TRUE iff the value is returned
 **/
polkit_bool_t
polkit_caller_get_selinux_context (PolKitCaller *caller, char **out_selinux_context)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        kit_return_val_if_fail (out_selinux_context != NULL, FALSE);
        *out_selinux_context = caller->selinux_context;
        return TRUE;
}

/**
 * polkit_caller_get_ck_session:
 * @caller: The caller object 
 * @out_session: Returns the session object. Caller shall not unref it.
 * 
 * Get the callers session. Note that this may be #NULL if the caller
 * is not in any session.
 * 
 * Returns: TRUE iff the value is returned
 **/
polkit_bool_t
polkit_caller_get_ck_session (PolKitCaller *caller, PolKitSession **out_session)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        kit_return_val_if_fail (out_session != NULL, FALSE);
        *out_session = caller->session;
        return TRUE;
}

/**
 * polkit_caller_debug:
 * @caller: the object
 * 
 * Print debug details
 **/
void
polkit_caller_debug (PolKitCaller *caller)
{
        kit_return_if_fail (caller != NULL);
        polkit_debug ("PolKitCaller: refcount=%d dbus_name=%s uid=%d pid=%d selinux_context=%s", 
                      caller->refcount, caller->dbus_name, caller->uid, caller->pid, caller->selinux_context);
        if (caller->session != NULL)
                polkit_session_debug (caller->session);
}


/**
 * polkit_caller_validate:
 * @caller: the object
 * 
 * Validate the object
 * 
 * Returns: #TRUE iff the object is valid.
 **/
polkit_bool_t
polkit_caller_validate (PolKitCaller *caller)
{
        kit_return_val_if_fail (caller != NULL, FALSE);
        kit_return_val_if_fail (caller->pid > 0, FALSE);
        return TRUE;
}

#ifdef POLKIT_BUILD_TESTS

static polkit_bool_t
_run_test (void)
{
        char *s;
        PolKitCaller *c;
        pid_t pid;
        uid_t uid;
        PolKitSeat *seat;
        PolKitSession *session;
        PolKitSession *session2;

        if ((c = polkit_caller_new ()) != NULL) {
                
                kit_assert (! polkit_caller_set_dbus_name (c, "org.invalid.name"));
                kit_assert (polkit_caller_set_dbus_name (c, NULL));
                if (polkit_caller_set_dbus_name (c, ":1.43")) {
                        kit_assert (polkit_caller_get_dbus_name (c, &s) && strcmp (s, ":1.43") == 0);

                        if (polkit_caller_set_dbus_name (c, ":1.44")) {
                                kit_assert (polkit_caller_get_dbus_name (c, &s) && strcmp (s, ":1.44") == 0);
                        }
                }

                kit_assert (polkit_caller_set_selinux_context (c, NULL));
                if (polkit_caller_set_selinux_context (c, "system_u:object_r:bin_t")) {
                        kit_assert (polkit_caller_get_selinux_context (c, &s) && strcmp (s, "system_u:object_r:bin_t") == 0);

                        if (polkit_caller_set_selinux_context (c, "system_u:object_r:httpd_exec_t")) {
                                kit_assert (polkit_caller_get_selinux_context (c, &s) && strcmp (s, "system_u:object_r:httpd_exec_t") == 0);
                        }
                }

                kit_assert (polkit_caller_set_uid (c, 0));
                kit_assert (polkit_caller_get_uid (c, &uid) && uid == 0);
                kit_assert (polkit_caller_set_pid (c, 1));
                kit_assert (polkit_caller_get_pid (c, &pid) && pid == 1);

                /* validate where caller is not in a session */
                kit_assert (polkit_caller_validate (c));
                polkit_caller_ref (c);
                kit_assert (polkit_caller_validate (c));
                polkit_caller_unref (c);
                kit_assert (polkit_caller_validate (c));

                if ((session = polkit_session_new ()) != NULL) {
                        if (polkit_session_set_ck_objref (session, "/somesession")) {
                                if ((seat = polkit_seat_new ()) != NULL) {
                                        if (polkit_seat_set_ck_objref (seat, "/someseat")) {
                                                kit_assert (polkit_session_set_seat (session, seat));
                                                kit_assert (polkit_session_set_ck_is_local (session, TRUE));

                                                kit_assert (polkit_caller_set_ck_session (c, NULL));
                                                kit_assert (polkit_caller_get_ck_session (c, &session2) && session2 == NULL);

                                                kit_assert (polkit_caller_set_ck_session (c, session));
                                                kit_assert (polkit_caller_set_ck_session (c, session));
                                                kit_assert (polkit_caller_get_ck_session (c, &session2) && session2 == session);
                                                /* validate where caller is in a session */
                                                kit_assert (polkit_caller_validate (c));

                                                polkit_caller_debug (c);


                                        }
                                        polkit_seat_unref (seat);
                                }
                        }
                        polkit_session_unref (session);
                }



                polkit_caller_unref (c);
        }        

        return TRUE;
}

KitTest _test_caller = {
        "polkit_caller",
        NULL,
        NULL,
        _run_test
};

#endif /* POLKIT_BUILD_TESTS */
