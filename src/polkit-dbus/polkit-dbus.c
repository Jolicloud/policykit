/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-dbus.h : helper library for obtaining seat, session and
 * caller information via D-Bus and ConsoleKit
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
 * SECTION:polkit-dbus
 * @title: Caller Determination
 * @short_description: Obtaining seat, session and caller information
 * via D-Bus and ConsoleKit.
 *
 * Helper library for obtaining seat, session and caller information
 * via D-Bus and ConsoleKit. This library is only useful when writing
 * a mechanism. 
 *
 * If the mechanism itself is a daemon exposing a remote services via
 * the system message bus it's often a better idea, to reduce
 * roundtrips, to use the high-level #PolKitTracker class rather than
 * the low-level functions polkit_caller_new_from_dbus_name() and
 * polkit_caller_new_from_pid().
 *
 * These functions are in <literal>libpolkit-dbus</literal>.
 **/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include "polkit-dbus.h"
#include <polkit/polkit-debug.h>
#include <polkit/polkit-test.h>
#include <polkit/polkit-private.h>

/**
 * polkit_session_new_from_objpath:
 * @con: D-Bus system bus connection
 * @objpath: object path of ConsoleKit session object
 * @uid: the user owning the session or -1 if unknown
 * @error: D-Bus error
 * 
 * This function will construct a #PolKitSession object by querying
 * the ConsoleKit daemon for information. Note that this will do a lot
 * of blocking IO so it is best avoided if your process already
 * tracks/caches all the information. If you pass in @uid as a
 * non-negative number, a round trip can be saved.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 * 
 * Returns: the new object or #NULL if an error occured (in which case
 * @error will be set)
 **/
PolKitSession *
polkit_session_new_from_objpath (DBusConnection *con, const char *objpath, uid_t uid, DBusError *error)
{
        PolKitSeat *seat;
        PolKitSession *session;
        DBusMessage *message;
        DBusMessage *reply;
        char *str;
        dbus_bool_t is_active;
        dbus_bool_t is_local;
        char *remote_host;
        char *seat_path;

        kit_return_val_if_fail (con != NULL, NULL);
        kit_return_val_if_fail (objpath != NULL, NULL);
        kit_return_val_if_fail (error != NULL, NULL);
        kit_return_val_if_fail (! dbus_error_is_set (error), NULL);

        session = NULL;
        remote_host = NULL;
        seat_path = NULL;

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						objpath,
						"org.freedesktop.ConsoleKit.Session",
						"IsActive");
	reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
	if (reply == NULL || dbus_error_is_set (error)) {
		kit_warning ("Error doing Session.IsActive on ConsoleKit: %s: %s", error->name, error->message);
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto out;
	}
	if (!dbus_message_get_args (reply, NULL,
				    DBUS_TYPE_BOOLEAN, &is_active,
                                    DBUS_TYPE_INVALID)) {
                kit_warning ("Invalid IsActive reply from CK");
		goto out;
	}
	dbus_message_unref (message);
	dbus_message_unref (reply);

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						objpath,
						"org.freedesktop.ConsoleKit.Session",
						"IsLocal");
	reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
	if (reply == NULL || dbus_error_is_set (error)) {
		kit_warning ("Error doing Session.IsLocal on ConsoleKit: %s: %s", error->name, error->message);
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto out;
	}
	if (!dbus_message_get_args (reply, NULL,
				    DBUS_TYPE_BOOLEAN, &is_local,
				    DBUS_TYPE_INVALID)) {
		kit_warning ("Invalid IsLocal reply from CK");
		goto out;
	}
	dbus_message_unref (message);
	dbus_message_unref (reply);

        if (!is_local) {
                message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
                                                        objpath,
                                                        "org.freedesktop.ConsoleKit.Session",
                                                        "GetRemoteHostName");
                reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
                if (reply == NULL || dbus_error_is_set (error)) {
                        kit_warning ("Error doing Session.GetRemoteHostName on ConsoleKit: %s: %s", 
                                   error->name, error->message);
                        dbus_message_unref (message);
                        if (reply != NULL)
                                dbus_message_unref (reply);
                        goto out;
                }
                if (!dbus_message_get_args (reply, NULL,
                                            DBUS_TYPE_STRING, &str,
                                            DBUS_TYPE_INVALID)) {
                        kit_warning ("Invalid GetRemoteHostName reply from CK");
                        goto out;
                }
                remote_host = kit_strdup (str);
                dbus_message_unref (message);
                dbus_message_unref (reply);
        }

        message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
                                                objpath,
                                                "org.freedesktop.ConsoleKit.Session",
                                                "GetSeatId");
        reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
        if (reply == NULL || dbus_error_is_set (error)) {
                kit_warning ("Error doing Session.GetSeatId on ConsoleKit: %s: %s", 
                           error->name, error->message);
                dbus_message_unref (message);
                if (reply != NULL)
                        dbus_message_unref (reply);
                goto out;
        }
        if (!dbus_message_get_args (reply, NULL,
                                    DBUS_TYPE_OBJECT_PATH, &str,
                                    DBUS_TYPE_INVALID)) {
                kit_warning ("Invalid GetSeatId reply from CK");
                goto out;
        }
        seat_path = kit_strdup (str);
        dbus_message_unref (message);
        dbus_message_unref (reply);

        if ((int) uid == -1) {
                message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
                                                        objpath,
                                                        "org.freedesktop.ConsoleKit.Session",
                                                        "GetUnixUser");
                reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
                if (reply == NULL || dbus_error_is_set (error)) {
                        kit_warning ("Error doing Session.GetUnixUser on ConsoleKit: %s: %s",error->name, error->message);
                        dbus_message_unref (message);
                        if (reply != NULL)
                                dbus_message_unref (reply);
                        goto out;
                }
                if (!dbus_message_get_args (reply, NULL,
                                            DBUS_TYPE_INT32, &uid,
                                            DBUS_TYPE_INVALID)) {
                        kit_warning ("Invalid GetUnixUser reply from CK");
                        goto out;
                }
                dbus_message_unref (message);
                dbus_message_unref (reply);
        }

        session = polkit_session_new ();
        if (session == NULL) {
                goto out;
        }
        if (!polkit_session_set_uid (session, uid)) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }
        if (!polkit_session_set_ck_objref (session, objpath)) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }
        if (!polkit_session_set_ck_is_active (session, is_active)) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }
        if (!polkit_session_set_ck_is_local (session, is_local)) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }
        if (!is_local) {
                if (!polkit_session_set_ck_remote_host (session, remote_host)) {
                        polkit_session_unref (session);
                        session = NULL;
                        goto out;
                }

        }

        seat = polkit_seat_new ();
        if (seat == NULL) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }
        if (!polkit_seat_set_ck_objref (seat, seat_path)) {
                polkit_seat_unref (seat);
                seat = NULL;
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }
        if (!polkit_seat_validate (seat)) {
                polkit_seat_unref (seat);
                seat = NULL;
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }

        if (!polkit_session_set_seat (session, seat)) {
                polkit_seat_unref (seat);
                seat = NULL;
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }
        polkit_seat_unref (seat); /* session object now owns this object */
        seat = NULL;

        if (!polkit_session_validate (session)) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }

out:
        kit_free (remote_host);
        kit_free (seat_path);
        return session;
}

/**
 * polkit_session_new_from_cookie:
 * @con: D-Bus system bus connection
 * @cookie: a ConsoleKit XDG_SESSION_COOKIE
 * @error: D-Bus error
 * 
 * This function will construct a #PolKitSession object by querying
 * the ConsoleKit daemon for information. Note that this will do a lot
 * of blocking IO so it is best avoided if your process already
 * tracks/caches all the information.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 * 
 * Returns: the new object or #NULL if an error occured (in which case
 * @error will be set)
 **/
PolKitSession *
polkit_session_new_from_cookie (DBusConnection *con, const char *cookie, DBusError *error)
{
        PolKitSession *session;
        DBusMessage *message;
        DBusMessage *reply;
        char *str;
        char *objpath;

        kit_return_val_if_fail (con != NULL, NULL);
        kit_return_val_if_fail (cookie != NULL, NULL);
        kit_return_val_if_fail (error != NULL, NULL);
        kit_return_val_if_fail (! dbus_error_is_set (error), NULL);

        objpath = NULL;
        session = NULL;

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						"/org/freedesktop/ConsoleKit/Manager",
						"org.freedesktop.ConsoleKit.Manager",
						"GetSessionForCookie");
	dbus_message_append_args (message, DBUS_TYPE_STRING, &cookie, DBUS_TYPE_INVALID);
	reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
	if (reply == NULL || dbus_error_is_set (error)) {
		//kit_warning ("Error doing Manager.GetSessionForCookie on ConsoleKit: %s: %s", error->name, error->message);
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto out;
	}
	if (!dbus_message_get_args (reply, NULL,
				    DBUS_TYPE_OBJECT_PATH, &str,
                                    DBUS_TYPE_INVALID)) {
                kit_warning ("Invalid GetSessionForCookie reply from CK");
		goto out;
	}
        objpath = kit_strdup (str);
	dbus_message_unref (message);
	dbus_message_unref (reply);

        session = polkit_session_new_from_objpath (con, objpath, -1, error);

out:
        kit_free (objpath);
        return session;
}


/**
 * polkit_caller_new_from_dbus_name:
 * @con: D-Bus system bus connection
 * @dbus_name: unique system bus connection name
 * @error: D-Bus error
 * 
 * This function will construct a #PolKitCaller object by querying
 * both the system bus daemon and the ConsoleKit daemon for
 * information. Note that this will do a lot of blocking IO so it is
 * best avoided if your process already tracks/caches all the
 * information. You can use the #PolKitTracker class for this.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 * 
 * Returns: the new object or #NULL if an error occured (in which case
 * @error will be set)
 **/
PolKitCaller *
polkit_caller_new_from_dbus_name (DBusConnection *con, const char *dbus_name, DBusError *error)
{
        PolKitCaller *caller;
        pid_t pid;
        uid_t uid;
        char *selinux_context;
        char *ck_session_objpath;
        PolKitSession *session;
        DBusMessage *message;
        DBusMessage *reply;
        DBusMessageIter iter;
        DBusMessageIter sub_iter;
        char *str;
        int num_elems;

        kit_return_val_if_fail (con != NULL, NULL);
        kit_return_val_if_fail (dbus_name != NULL, NULL);
        kit_return_val_if_fail (error != NULL, NULL);
        kit_return_val_if_fail (! dbus_error_is_set (error), NULL);

        selinux_context = NULL;
        ck_session_objpath = NULL;

        caller = NULL;
        session = NULL;

	uid = dbus_bus_get_unix_user (con, dbus_name, error);
	if (dbus_error_is_set (error)) {
		kit_warning ("Could not get uid for connection: %s %s", error->name, error->message);
		goto out;
	}

	message = dbus_message_new_method_call ("org.freedesktop.DBus", 
						"/org/freedesktop/DBus/Bus",
						"org.freedesktop.DBus",
						"GetConnectionUnixProcessID");
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &dbus_name);
	reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
	if (reply == NULL || dbus_error_is_set (error)) {
		kit_warning ("Error doing GetConnectionUnixProcessID on Bus: %s: %s", error->name, error->message);
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto out;
	}
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_get_basic (&iter, &pid);
	dbus_message_unref (message);
	dbus_message_unref (reply);

	message = dbus_message_new_method_call ("org.freedesktop.DBus", 
						"/org/freedesktop/DBus/Bus",
						"org.freedesktop.DBus",
						"GetConnectionSELinuxSecurityContext");
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &dbus_name);
	reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
        /* SELinux might not be enabled */
        if (dbus_error_is_set (error) && 
            strcmp (error->name, "org.freedesktop.DBus.Error.SELinuxSecurityContextUnknown") == 0) {
                dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
                dbus_error_init (error);
        } else if (reply == NULL || dbus_error_is_set (error)) {
                kit_warning ("Error doing GetConnectionSELinuxSecurityContext on Bus: %s: %s", error->name, error->message);
                dbus_message_unref (message);
                if (reply != NULL)
                        dbus_message_unref (reply);
                goto out;
        } else {
                /* TODO: verify signature */
                dbus_message_iter_init (reply, &iter);
                dbus_message_iter_recurse (&iter, &sub_iter);
                dbus_message_iter_get_fixed_array (&sub_iter, (void *) &str, &num_elems);
                if (str != NULL && num_elems > 0)
                        selinux_context = kit_strndup (str, num_elems);
                dbus_message_unref (message);
                dbus_message_unref (reply);
        }

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						"/org/freedesktop/ConsoleKit/Manager",
						"org.freedesktop.ConsoleKit.Manager",
						"GetSessionForUnixProcess");
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &pid);
	reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
	if (reply == NULL || dbus_error_is_set (error)) {
		//kit_warning ("Error doing GetSessionForUnixProcess on ConsoleKit: %s: %s", error->name, error->message);
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		/* OK, this is not a catastrophe; just means the caller is not a 
                 * member of any session or that ConsoleKit is not available.. 
                 */
		goto not_in_session;
	}
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_get_basic (&iter, &str);
	ck_session_objpath = kit_strdup (str);
	dbus_message_unref (message);
	dbus_message_unref (reply);

        session = polkit_session_new_from_objpath (con, ck_session_objpath, uid, error);
        if (session == NULL) {
                kit_warning ("Got a session objpath but couldn't construct session object!");
                goto out;
        }
        if (!polkit_session_validate (session)) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }

not_in_session:

        caller = polkit_caller_new ();
        if (caller == NULL) {
                if (session != NULL) {
                        polkit_session_unref (session);
                        session = NULL;
                }
                goto out;
        }

        if (!polkit_caller_set_dbus_name (caller, dbus_name)) {
                if (session != NULL) {
                        polkit_session_unref (session);
                        session = NULL;
                }
                polkit_caller_unref (caller);
                caller = NULL;
                goto out;
        }
        if (!polkit_caller_set_uid (caller, uid)) {
                if (session != NULL) {
                        polkit_session_unref (session);
                        session = NULL;
                }
                polkit_caller_unref (caller);
                caller = NULL;
                goto out;
        }
        if (!polkit_caller_set_pid (caller, pid)) {
                if (session != NULL) {
                        polkit_session_unref (session);
                        session = NULL;
                }
                polkit_caller_unref (caller);
                caller = NULL;
                goto out;
        }
        if (selinux_context != NULL) {
                if (!polkit_caller_set_selinux_context (caller, selinux_context)) {
                        if (session != NULL) {
                                polkit_session_unref (session);
                                session = NULL;
                        }
                        polkit_caller_unref (caller);
                        caller = NULL;
                        goto out;
                }
        }
        if (session != NULL) {
                if (!polkit_caller_set_ck_session (caller, session)) {
                        if (session != NULL) {
                                polkit_session_unref (session);
                                session = NULL;
                        }
                        polkit_caller_unref (caller);
                        caller = NULL;
                        goto out;
                }
                polkit_session_unref (session); /* caller object now own this object */
                session = NULL;
        }

        if (!polkit_caller_validate (caller)) {
                polkit_caller_unref (caller);
                caller = NULL;
                goto out;
        }

out:
        kit_free (selinux_context);
        kit_free (ck_session_objpath);
        return caller;
}

/**
 * polkit_caller_new_from_pid:
 * @con: D-Bus system bus connection
 * @pid: process id
 * @error: D-Bus error
 * 
 * This function will construct a #PolKitCaller object by querying
 * both information in /proc (on Linux) and the ConsoleKit daemon for
 * information about a given process. Note that this will do a lot of
 * blocking IO so it is best avoided if your process already
 * tracks/caches all the information. You can use the #PolKitTracker
 * class for this.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 * 
 * Returns: the new object or #NULL if an error occured (in which case
 * @error will be set)
 **/
PolKitCaller *
polkit_caller_new_from_pid (DBusConnection *con, pid_t pid, DBusError *error)
{
        PolKitCaller *caller;
        uid_t uid;
        char *selinux_context;
        char *ck_session_objpath;
        PolKitSession *session;
        DBusMessage *message;
        DBusMessage *reply;
        DBusMessageIter iter;
        char *str;
        char *proc_path;
        struct stat statbuf;
#ifdef HAVE_SELINUX
        security_context_t secon;
#endif

#ifndef POLKIT_BUILD_TESTS
        /* for testing it's fine to pass con==NULL if POLKIT_TEST_PRETEND_TO_BE_CK_SESSION_OBJPATH is set */
        kit_return_val_if_fail (con != NULL, NULL);
#endif
        kit_return_val_if_fail (error != NULL, NULL);
        kit_return_val_if_fail (! dbus_error_is_set (error), NULL);

        selinux_context = NULL;
        ck_session_objpath = NULL;
        uid = (uid_t) -1;
        caller = NULL;
        session = NULL;
        proc_path = NULL;

#ifdef POLKIT_BUILD_TESTS
        char *pretend;
        if ((pretend = getenv ("POLKIT_TEST_PRETEND_TO_BE_UID")) != NULL) {
                uid = atoi (pretend);
        }
        if ((pretend = getenv ("POLKIT_TEST_PRETEND_TO_BE_PID")) != NULL) {
                pid = atoi (pretend);
        }
        if ((pretend = getenv ("POLKIT_TEST_PRETEND_TO_BE_SELINUX_CONTEXT")) != NULL) {
                selinux_context = kit_strdup (pretend);
        }
        if ((pretend = getenv ("POLKIT_TEST_PRETEND_TO_BE_CK_SESSION_OBJPATH")) != NULL) {
                ck_session_objpath = kit_strdup (pretend);
        } else {
                kit_return_val_if_fail (con != NULL, NULL);
        }
#endif

        if (uid == (uid_t) -1) {
                proc_path = kit_strdup_printf ("/proc/%d", pid);
                if (proc_path && stat (proc_path, &statbuf) != 0) {
                        kit_warning ("Cannot lookup information for pid %d: %s", pid, strerror (errno));
                        goto out;
                }
                uid = statbuf.st_uid;
        }

#ifdef HAVE_SELINUX
	/* only get the context if we are enabled */
        if (selinux_context == NULL) {
                if (is_selinux_enabled () != 0) {
                        if (getpidcon (pid, &secon) != 0) {
                                kit_warning ("Cannot lookup SELinux context for pid %d: %s", pid, strerror (errno));
                                goto out;
                        }
                        selinux_context = kit_strdup (secon);
                        freecon (secon);
                }
        }
#else
        selinux_context = NULL;
#endif

        if (ck_session_objpath == NULL) {
                message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
                                                        "/org/freedesktop/ConsoleKit/Manager",
                                                        "org.freedesktop.ConsoleKit.Manager",
                                                        "GetSessionForUnixProcess");
                dbus_message_iter_init_append (message, &iter);
                dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &pid);
                reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
                if (reply == NULL || dbus_error_is_set (error)) {
                        //kit_warning ("Error doing GetSessionForUnixProcess on ConsoleKit: %s: %s", error->name, error->message);
                        dbus_message_unref (message);
                        if (reply != NULL)
                                dbus_message_unref (reply);
                        /* OK, this is not a catastrophe; just means the caller is not a 
                         * member of any session or that ConsoleKit is not available.. 
                         */
                        goto not_in_session;
                }
                dbus_message_iter_init (reply, &iter);
                dbus_message_iter_get_basic (&iter, &str);
                ck_session_objpath = kit_strdup (str);
                dbus_message_unref (message);
                dbus_message_unref (reply);
        } else {
                if (strlen (ck_session_objpath) == 0)
                        goto not_in_session;
        }

        session = polkit_session_new_from_objpath (con, ck_session_objpath, uid, error);
        if (session == NULL) {
                kit_warning ("Got a session objpath but couldn't construct session object!");
                goto out;
        }
        if (!polkit_session_validate (session)) {
                polkit_session_unref (session);
                session = NULL;
                goto out;
        }

not_in_session:

        caller = polkit_caller_new ();
        if (caller == NULL) {
                if (session != NULL) {
                        polkit_session_unref (session);
                        session = NULL;
                }
                goto out;
        }

        if (!polkit_caller_set_uid (caller, uid)) {
                if (session != NULL) {
                        polkit_session_unref (session);
                        session = NULL;
                }
                polkit_caller_unref (caller);
                caller = NULL;
                goto out;
        }

        if (!polkit_caller_set_pid (caller, pid)) {
                if (session != NULL) {
                        polkit_session_unref (session);
                        session = NULL;
                }
                polkit_caller_unref (caller);
                caller = NULL;
                goto out;
        }
        if (selinux_context != NULL) {
                if (!polkit_caller_set_selinux_context (caller, selinux_context)) {
                        if (session != NULL) {
                                polkit_session_unref (session);
                                session = NULL;
                        }
                        polkit_caller_unref (caller);
                        caller = NULL;
                        goto out;
                }
        }
        if (session != NULL) {
                if (!polkit_caller_set_ck_session (caller, session)) {
                        if (session != NULL) {
                                polkit_session_unref (session);
                                session = NULL;
                        }
                        polkit_caller_unref (caller);
                        caller = NULL;
                        goto out;
                }
                polkit_session_unref (session); /* caller object now own this object */
                session = NULL;
        }

        if (!polkit_caller_validate (caller)) {
                polkit_caller_unref (caller);
                caller = NULL;
                goto out;
        }

out:
        kit_free (selinux_context);
        kit_free (ck_session_objpath);
        kit_free (proc_path);
        return caller;
}

static kit_bool_t
_free_elem_in_list (void *data, void *user_data, KitList *list)
{
        kit_free (data);
        return FALSE;
}

static KitList *
_get_list_of_sessions (DBusConnection *con, uid_t uid, DBusError *error)
{
        KitList *ret;
        DBusMessage *message;
        DBusMessage *reply;
        DBusMessageIter iter;
        DBusMessageIter iter_array;
        const char *value;

        ret = NULL;

        message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
                                                "/org/freedesktop/ConsoleKit/Manager",
                                                "org.freedesktop.ConsoleKit.Manager",
                                                "GetSessionsForUnixUser");
	dbus_message_append_args (message, DBUS_TYPE_UINT32, &uid, DBUS_TYPE_INVALID);
        reply = dbus_connection_send_with_reply_and_block (con, message, -1, error);
        if (reply == NULL || dbus_error_is_set (error)) {
                goto out;
        }

	dbus_message_iter_init (reply, &iter);
	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) {
                kit_warning ("Wrong reply from ConsoleKit (not an array)");
                goto out;
	}

	dbus_message_iter_recurse (&iter, &iter_array);
        while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID) {

                if (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_OBJECT_PATH) {
                        kit_warning ("Wrong reply from ConsoleKit (element is not a string)");
                        kit_list_foreach (ret, _free_elem_in_list, NULL);
                        kit_list_free (ret);
                        goto out;
                }

		dbus_message_iter_get_basic (&iter_array, &value);
                ret = kit_list_append (ret, kit_strdup (value));

		dbus_message_iter_next (&iter_array);
        }
        
out:
        if (message != NULL)
                dbus_message_unref (message);
        if (reply != NULL)
                dbus_message_unref (reply);
        return ret;
}

static polkit_bool_t
_polkit_is_authorization_relevant_internal (DBusConnection *con, 
                                            PolKitAuthorization *auth, 
                                            KitList *sessions,
                                            DBusError *error)
{
        pid_t pid;
        polkit_uint64_t pid_start_time;
        polkit_bool_t ret;
        polkit_bool_t del_sessions;
        KitList *i;
        uid_t uid;

        kit_return_val_if_fail (con != NULL, FALSE);
        kit_return_val_if_fail (auth != NULL, FALSE);
        kit_return_val_if_fail (error != NULL, FALSE);
        kit_return_val_if_fail (! dbus_error_is_set (error), FALSE);

        ret = FALSE;

        uid = polkit_authorization_get_uid (auth);

        switch (polkit_authorization_get_scope (auth)) {
        case POLKIT_AUTHORIZATION_SCOPE_PROCESS_ONE_SHOT:
        case POLKIT_AUTHORIZATION_SCOPE_PROCESS:
                if (!polkit_authorization_scope_process_get_pid (auth,
                                                                 &pid,
                                                                 &pid_start_time)) {
                        /* this should never fail */
                        kit_warning ("Cannot determine (pid,start_time) for authorization");
                        goto out;
                }
                if (polkit_sysdeps_get_start_time_for_pid (pid) == pid_start_time) {
                        ret = TRUE;
                        goto out;
                }
                break;

        case POLKIT_AUTHORIZATION_SCOPE_SESSION:
                del_sessions = FALSE;
                if (sessions == NULL) {
                        sessions = _get_list_of_sessions (con, uid, error);
                        del_sessions = TRUE;
                }

                if (sessions != NULL) {
                        for (i = sessions; i != NULL; i = i->next) {
                                char *session_id = i->data;
                                if (strcmp (session_id, polkit_authorization_scope_session_get_ck_objref (auth)) == 0) {
                                        ret = TRUE;
                                        break;
                                }
                        }
                        
                        if (del_sessions) {
                                kit_list_foreach (sessions, _free_elem_in_list, NULL);
                                kit_list_free (sessions);
                        }
                }
                break;

        case POLKIT_AUTHORIZATION_SCOPE_ALWAYS:
                ret = TRUE;
                break;
        }

out:
        return ret;
}

/**
 * polkit_is_authorization_relevant:
 * @con: D-Bus system bus connection
 * @auth: authorization to check for
 * @error: return location for error
 *
 * As explicit authorizations are scoped (process single shot,
 * process, session or everything), they become irrelevant once the
 * entity (process or session) ceases to exist. This function
 * determines whether the authorization is still relevant; it's useful
 * for reporting and graphical tools displaying authorizations.
 *
 * Note that this may do blocking IO to check for session
 * authorizations so it is best avoided if your process already
 * tracks/caches all the information. You can use the
 * polkit_tracker_is_authorization_relevant() method on the
 * #PolKitTracker class for this.
 *
 * Returns: #TRUE if the authorization still applies, #FALSE if an
 * error occurred (then error will be set) or if the entity the
 * authorization refers to has gone out of scope.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_is_authorization_relevant (DBusConnection *con, PolKitAuthorization *auth, DBusError *error)
{
        return _polkit_is_authorization_relevant_internal (con, auth, NULL, error);
}

/**
 * PolKitTracker:
 *
 * Instances of this class are used to cache information about
 * callers; typically this is used in scenarios where the same caller
 * is calling into a mechanism multiple times. 
 *
 * Thus, an application can use this class to get the #PolKitCaller
 * object; the class will listen to both NameOwnerChanged and
 * ActivityChanged signals from the message bus and update / retire
 * the #PolKitCaller objects.
 *
 * An example of how to use #PolKitTracker is provided here. First, build the following program
 *
 * <programlisting><xi:include xmlns:xi="http://www.w3.org/2001/XInclude" href="../../examples/tracker-example/tracker-example.c" parse="text"><xi:fallback>FIXME: MISSING XINCLUDE CONTENT</xi:fallback></xi:include></programlisting>
 *
 * with
 *
 * <programlisting>gcc -o tracker-example `pkg-config --cflags --libs dbus-glib-1 polkit-dbus` tracker-example.c</programlisting>
 *
 * Then put the following content
 *
 * <programlisting><xi:include xmlns:xi="http://www.w3.org/2001/XInclude" href="../../examples/tracker-example/dk.fubar.PolKitTestService.conf" parse="text"><xi:fallback>FIXME: MISSING XINCLUDE CONTENT</xi:fallback></xi:include></programlisting>
 *
 * in the file <literal>/etc/dbus-1/system.d/dk.fubar.PolKitTestService.conf</literal>. Finally,
 * create a small Python client like this
 *
 * <programlisting><xi:include xmlns:xi="http://www.w3.org/2001/XInclude" href="../../examples/tracker-example/tracker-example-client.py" parse="text"><xi:fallback>FIXME: MISSING XINCLUDE CONTENT</xi:fallback></xi:include></programlisting>
 *
 * as <literal>tracker-example-client.py</literal>. Now, run <literal>tracker-example</literal>
 * in one window and <literal>tracker-example-client</literal> in another. The output of
 * the former should look like this
 *
 *
 * <programlisting>
 * 18:20:00.414: PolKitCaller: refcount=1 dbus_name=:1.473 uid=500 pid=8636 selinux_context=system_u:system_r:unconfined_t
 * 18:20:00.414: PolKitSession: refcount=1 uid=0 objpath=/org/freedesktop/ConsoleKit/Session1 is_active=1 is_local=1 remote_host=(null)
 * 18:20:00.414: PolKitSeat: refcount=1 objpath=/org/freedesktop/ConsoleKit/Seat1
 * 
 * 18:20:01.424: PolKitCaller: refcount=1 dbus_name=:1.473 uid=500 pid=8636 selinux_context=system_u:system_r:unconfined_t
 * 18:20:01.424: PolKitSession: refcount=1 uid=0 objpath=/org/freedesktop/ConsoleKit/Session1 is_active=1 is_local=1 remote_host=(null)
 * 18:20:01.424: PolKitSeat: refcount=1 objpath=/org/freedesktop/ConsoleKit/Seat1
 * 
 * 18:20:02.434: PolKitCaller: refcount=1 dbus_name=:1.473 uid=500 pid=8636 selinux_context=system_u:system_r:unconfined_t
 * 18:20:02.434: PolKitSession: refcount=1 uid=0 objpath=/org/freedesktop/ConsoleKit/Session1 is_active=0 is_local=1 remote_host=(null)
 * 18:20:02.434: PolKitSeat: refcount=1 objpath=/org/freedesktop/ConsoleKit/Seat1
 * 
 * 18:20:03.445: PolKitCaller: refcount=1 dbus_name=:1.473 uid=500 pid=8636 selinux_context=system_u:system_r:unconfined_t
 * 18:20:03.445: PolKitSession: refcount=1 uid=0 objpath=/org/freedesktop/ConsoleKit/Session1 is_active=1 is_local=1 remote_host=(null)
 * 18:20:03.445: PolKitSeat: refcount=1 objpath=/org/freedesktop/ConsoleKit/Seat1
 * </programlisting>
 *
 * The point of the test program is simply to gather caller
 * information about clients (the small Python program, you may launch
 * multiple instances of it) that repeatedly calls into the D-Bus
 * service; if one runs <literal>strace(1)</literal> in front of the
 * test program one will notice that there is only syscall / IPC
 * overhead (except for printing to stdout) on the first call from the
 * client.
 *
 * The careful reader will notice that, during the testing session, we
 * did a quick VT switch away from the session (and back) which is
 * reflected in the output.
 *
 * These functions are in <literal>libpolkit-dbus</literal>.
 **/
struct _PolKitTracker {
        int refcount;
        DBusConnection *con;

        KitHash *dbus_name_to_caller;

        KitHash *pid_start_time_to_caller;
};

typedef struct {
        pid_t pid;
        polkit_uint64_t start_time;
} _PidStartTimePair;

static _PidStartTimePair *
_pid_start_time_new (pid_t pid, polkit_uint64_t start_time)
{
        _PidStartTimePair *obj;
        obj = kit_new (_PidStartTimePair, 1);
        obj->pid = pid;
        obj->start_time = start_time;
        return obj;
}

static uint32_t
_pid_start_time_hash (const void *a)
{
        uint32_t val;
        _PidStartTimePair *pst = (_PidStartTimePair *) a;

        val = pst->pid + ((int) pst->start_time);

        return val;
}

static kit_bool_t
_pid_start_time_equal (const void *a, const void *b)
{
        _PidStartTimePair *_a = (_PidStartTimePair *) a;
        _PidStartTimePair *_b = (_PidStartTimePair *) b;

        return (_a->pid == _b->pid) && (_a->start_time == _b->start_time);
}

/**
 * polkit_tracker_new:
 * 
 * Creates a new #PolKitTracker object.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 * 
 * Returns: the new object
 *
 * Since: 0.7
 **/
PolKitTracker *
polkit_tracker_new (void)
{
        PolKitTracker *pk_tracker;
        pk_tracker = kit_new0 (PolKitTracker, 1);
        pk_tracker->refcount = 1;
        pk_tracker->dbus_name_to_caller = kit_hash_new (kit_hash_str_hash_func, 
                                                        kit_hash_str_equal_func,
                                                        NULL,
                                                        NULL,
                                                        (KitFreeFunc) kit_free,
                                                        (KitFreeFunc) polkit_caller_unref);
        pk_tracker->pid_start_time_to_caller = kit_hash_new (_pid_start_time_hash,
                                                             _pid_start_time_equal,
                                                             NULL,
                                                             NULL,
                                                             (KitFreeFunc) kit_free,
                                                             (KitFreeFunc) polkit_caller_unref);
        return pk_tracker;
}

/**
 * polkit_tracker_ref:
 * @pk_tracker: the tracker object
 * 
 * Increase reference count.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 * 
 * Returns: the object
 *
 * Since: 0.7
 **/
PolKitTracker *
polkit_tracker_ref (PolKitTracker *pk_tracker)
{
        kit_return_val_if_fail (pk_tracker != NULL, pk_tracker);
        pk_tracker->refcount++;
        return pk_tracker;
}

/**
 * polkit_tracker_unref:
 * @pk_tracker: the tracker object
 * 
 * Decreases the reference count of the object. If it becomes zero,
 * the object is freed. Before freeing, reference counts on embedded
 * objects are decresed by one.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Since: 0.7
 **/
void
polkit_tracker_unref (PolKitTracker *pk_tracker)
{
        kit_return_if_fail (pk_tracker != NULL);
        pk_tracker->refcount--;
        if (pk_tracker->refcount > 0) 
                return;
        kit_hash_unref (pk_tracker->dbus_name_to_caller);
        kit_hash_unref (pk_tracker->pid_start_time_to_caller);
        dbus_connection_unref (pk_tracker->con);
        kit_free (pk_tracker);
}

/**
 * polkit_tracker_set_system_bus_connection:
 * @pk_tracker: the tracker object
 * @con: the connection to the system message bus
 * 
 * Tell the #PolKitTracker object to use the given D-Bus connection
 * when it needs to fetch information from the system message bus and
 * ConsoleKit services. This is used for priming the cache.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Since: 0.7
 */
void
polkit_tracker_set_system_bus_connection (PolKitTracker *pk_tracker, DBusConnection *con)
{
        kit_return_if_fail (pk_tracker != NULL);
        pk_tracker->con = dbus_connection_ref (con);
}

/**
 * polkit_tracker_init:
 * @pk_tracker: the tracker object
 * 
 * Initialize the tracker.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Since: 0.7
 */
void
polkit_tracker_init (PolKitTracker *pk_tracker)
{
        kit_return_if_fail (pk_tracker != NULL);
        /* This is currently a no-op */
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
_set_session_inactive_iter (void *key, PolKitCaller *caller, const char *session_objpath, KitHash *hash)
{
        char *objpath;
        PolKitSession *session;
        if (!polkit_caller_get_ck_session (caller, &session))
                return;
        if (!polkit_session_get_ck_objref (session, &objpath))
                return;
        if (strcmp (objpath, session_objpath) != 0)
                return;
        polkit_session_set_ck_is_active (session, FALSE);
}

static void
_set_session_active_iter (void *key, PolKitCaller *caller, const char *session_objpath, KitHash *hash)
{
        char *objpath;
        PolKitSession *session;
        if (!polkit_caller_get_ck_session (caller, &session))
                return;
        if (!polkit_session_get_ck_objref (session, &objpath))
                return;
        if (strcmp (objpath, session_objpath) != 0)
                return;
        polkit_session_set_ck_is_active (session, TRUE);
}

static void
_update_session_is_active (PolKitTracker *pk_tracker, const char *session_objpath, kit_bool_t is_active)
{
        kit_hash_foreach (pk_tracker->dbus_name_to_caller,
                          (KitHashForeachFunc) (is_active ? _set_session_active_iter : _set_session_inactive_iter),
                          (void *) session_objpath);
}

/*--------------------------------------------------------------------------------------------------------------*/

static kit_bool_t
_remove_caller_by_session_iter (void *key, PolKitCaller *caller, const char *session_objpath, KitHash *hash)
{
        char *objpath;
        PolKitSession *session;
        if (!polkit_caller_get_ck_session (caller, &session))
                return FALSE;
        if (!polkit_session_get_ck_objref (session, &objpath))
                return FALSE;
        if (strcmp (objpath, session_objpath) != 0)
                return FALSE;
        return TRUE;
}

static void
_remove_caller_by_session (PolKitTracker *pk_tracker, const char *session_objpath)
{
        kit_hash_foreach_remove (pk_tracker->dbus_name_to_caller, 
                                 (KitHashForeachFunc) _remove_caller_by_session_iter,
                                 (void *) session_objpath);
}

/*--------------------------------------------------------------------------------------------------------------*/

static kit_bool_t
_remove_caller_by_dbus_name_iter (void *key, PolKitCaller *caller, const char *dbus_name, KitHash *hash)
{
        char *name;
        if (!polkit_caller_get_dbus_name (caller, &name))
                return FALSE;
        if (strcmp (name, dbus_name) != 0)
                return FALSE;
        return TRUE;
}

static void
_remove_caller_by_dbus_name (PolKitTracker *pk_tracker, const char *dbus_name)
{
        kit_hash_foreach_remove (pk_tracker->dbus_name_to_caller,
                                 (KitHashForeachFunc) _remove_caller_by_dbus_name_iter,
                                 (void *) dbus_name);
}

/*--------------------------------------------------------------------------------------------------------------*/

/**
 * polkit_tracker_dbus_func:
 * @pk_tracker: the tracker object
 * @message: message to pass
 * 
 * The owner of the #PolKitTracker object must pass signals from the
 * system message bus (just NameOwnerChanged will do) and all signals
 * from the ConsoleKit service into this function.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Returns: #TRUE only if there was a change in the ConsoleKit database.
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_tracker_dbus_func (PolKitTracker *pk_tracker, DBusMessage *message)
{
        kit_bool_t ret;

        ret = FALSE;

        if (dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
		char *name;
		char *new_service_name;
		char *old_service_name;
                
		if (!dbus_message_get_args (message, NULL,
					    DBUS_TYPE_STRING, &name,
					    DBUS_TYPE_STRING, &old_service_name,
					    DBUS_TYPE_STRING, &new_service_name,
					    DBUS_TYPE_INVALID)) {

                        /* TODO: should be _pk_critical */
                        polkit_debug ("The NameOwnerChanged signal on the " DBUS_INTERFACE_DBUS " "
                                      "interface has the wrong signature! Your system is misconfigured.");
			goto out;
		}

                if (strlen (new_service_name) == 0) {
                        _remove_caller_by_dbus_name (pk_tracker, name);
                }

        } else if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Session", "ActiveChanged")) {
                dbus_bool_t is_active;
                DBusError error;
                const char *session_objpath;

                ret = TRUE;

                dbus_error_init (&error);
                session_objpath = dbus_message_get_path (message);
                if (!dbus_message_get_args (message, &error, 
                                            DBUS_TYPE_BOOLEAN, &is_active, 
                                            DBUS_TYPE_INVALID)) {

                        /* TODO: should be _pk_critical */
                        kit_warning ("The ActiveChanged signal on the org.freedesktop.ConsoleKit.Session "
                                   "interface for object %s has the wrong signature! "
                                   "Your system is misconfigured.", session_objpath);

                        /* as a security measure, remove all sessions with this path from the cache;
                         * cuz then the user of PolKitTracker probably gets to deal with a DBusError
                         * the next time he tries something...
                         */
                        _remove_caller_by_session (pk_tracker, session_objpath);
                        goto out;
                }

                /* now go through all Caller objects and update the is_active field as appropriate */
                _update_session_is_active (pk_tracker, session_objpath, is_active);

        } else if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Seat", "SessionAdded")) {
                DBusError error;
                const char *seat_objpath;
                const char *session_objpath;

                /* If a session is added, update our list of sessions.. also notify the user.. */

                ret = TRUE;

                dbus_error_init (&error);
                seat_objpath = dbus_message_get_path (message);
                if (!dbus_message_get_args (message, &error, 
                                            DBUS_TYPE_STRING, &session_objpath, 
                                            DBUS_TYPE_INVALID)) {

                        /* TODO: should be _pk_critical */
                        kit_warning ("The SessionAdded signal on the org.freedesktop.ConsoleKit.Seat "
                                   "interface for object %s has the wrong signature! "
                                   "Your system is misconfigured.", seat_objpath);

                        goto out;
                }

                /* TODO: add to sessions - see polkit_tracker_is_authorization_relevant() */

        } else if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Seat", "SessionRemoved")) {
                DBusError error;
                const char *seat_objpath;
                const char *session_objpath;

                /* If a session is removed, authorizations scoped for that session 
                 * may become inactive.. so do notify the user about it.. 
                 */

                ret = TRUE;

                dbus_error_init (&error);
                seat_objpath = dbus_message_get_path (message);
                if (!dbus_message_get_args (message, &error, 
                                            DBUS_TYPE_STRING, &session_objpath, 
                                            DBUS_TYPE_INVALID)) {

                        /* TODO: should be _pk_critical */
                        kit_warning ("The SessionRemoved signal on the org.freedesktop.ConsoleKit.Seat "
                                   "interface for object %s has the wrong signature! "
                                   "Your system is misconfigured.", seat_objpath);

                        goto out;
                }

                _remove_caller_by_session (pk_tracker, session_objpath);

                /* TODO: remove from sessions - see polkit_tracker_is_authorization_relevant() */
        }

        /* TODO: when ConsoleKit gains the ability to attach/detach a session to a seat (think
         * hot-desking), we want to update our local caches too 
         */

out:
        return ret;
}

/**
 * polkit_tracker_get_caller_from_dbus_name:
 * @pk_tracker: the tracker object
 * @dbus_name: unique name on the system message bus
 * @error: D-Bus error
 *
 * This function is similar to polkit_caller_new_from_dbus_name()
 * except that it uses the cache in #PolKitTracker. So on the second
 * and subsequent calls, for the same D-Bus name, there will be no
 * syscall or IPC overhead in calling this function.
 * 
 * Returns: A #PolKitCaller object; the caller must use
 * polkit_caller_unref() on the object when done with it. Returns
 * #NULL if an error occured (in which case error will be set).
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Since: 0.7
 */
PolKitCaller *
polkit_tracker_get_caller_from_dbus_name (PolKitTracker *pk_tracker, const char *dbus_name, DBusError *error)
{
        PolKitCaller *caller;

        kit_return_val_if_fail (pk_tracker != NULL, NULL);
        kit_return_val_if_fail (pk_tracker->con != NULL, NULL);
        kit_return_val_if_fail (! dbus_error_is_set (error), NULL);

        /* kit_debug ("Looking up cache for PolKitCaller for dbus_name %s...", dbus_name); */

        caller = kit_hash_lookup (pk_tracker->dbus_name_to_caller, (void *) dbus_name, NULL);
        if (caller != NULL)
                return polkit_caller_ref (caller);

        /* kit_debug ("Have to compute PolKitCaller for dbus_name %s...", dbus_name); */

        caller = polkit_caller_new_from_dbus_name (pk_tracker->con, dbus_name, error);
        if (caller == NULL)
                return NULL;

        kit_hash_insert (pk_tracker->dbus_name_to_caller, kit_strdup (dbus_name), caller);
        return polkit_caller_ref (caller);
}


/**
 * polkit_tracker_get_caller_from_pid:
 * @pk_tracker: the tracker object
 * @pid: UNIX process id to look at
 * @error: D-Bus error
 *
 * This function is similar to polkit_caller_new_from_pid()
 * except that it uses the cache in #PolKitTracker. So on the second
 * and subsequent calls, for the same D-Bus name, there will be no
 * IPC overhead in calling this function. 
 *
 * There will be some syscall overhead to lookup the time when the
 * given process is started (on Linux, looking up /proc/$pid/stat);
 * this is needed because pid's can be recycled and the cache thus
 * needs to record this in addition to the pid.
 * 
 * Returns: A #PolKitCaller object; the caller must use
 * polkit_caller_unref() on the object when done with it. Returns
 * #NULL if an error occured (in which case error will be set).
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Since: 0.7
 */
PolKitCaller *
polkit_tracker_get_caller_from_pid (PolKitTracker *pk_tracker, pid_t pid, DBusError *error)
{
        PolKitCaller *caller;
        polkit_uint64_t start_time;
        _PidStartTimePair *pst;

        kit_return_val_if_fail (pk_tracker != NULL, NULL);
        kit_return_val_if_fail (pk_tracker->con != NULL, NULL);
        kit_return_val_if_fail (! dbus_error_is_set (error), NULL);

        start_time = polkit_sysdeps_get_start_time_for_pid (pid);
        if (start_time == 0) {
                if (error != NULL) {
                        dbus_set_error (error, 
                                        "org.freedesktop.PolicyKit",
                                        "Cannot look up start time for pid %d", pid);
                }
                return NULL;
        }

        pst = _pid_start_time_new (pid, start_time);

        /* kit_debug ("Looking up cache for pid %d (start_time %lld)...", pid, start_time); */

        caller = kit_hash_lookup (pk_tracker->pid_start_time_to_caller, (void *) pst, NULL);
        if (caller != NULL) {
                kit_free (pst);
                return polkit_caller_ref (caller);
        }

        /* kit_debug ("Have to compute PolKitCaller from pid %d (start_time %lld)...", pid, start_time); */

        caller = polkit_caller_new_from_pid (pk_tracker->con, pid, error);
        if (caller == NULL) {
                kit_free (pst);
                return NULL;
        }

        /* TODO: we need to evict old entries.. 
         *
         * Say, timestamp the entries in _PidStartTimePair and do
         * garbage collection every hour or so (e.g. record when we
         * last did garbage collection and check this time on the next
         * call into this function).
         */

        kit_hash_insert (pk_tracker->pid_start_time_to_caller, pst, caller);
        return polkit_caller_ref (caller);
}


/**
 * polkit_tracker_is_authorization_relevant:
 * @pk_tracker: the tracker
 * @auth: authorization to check for
 * @error: return location for error
 *
 * As explicit authorizations are scoped (process single shot,
 * process, session or everything), they become irrelevant once the
 * entity (process or session) ceases to exist. This function
 * determines whether the authorization is still relevant; it's useful
 * for reporting and graphical tools displaying authorizations.
 *
 * This function is similar to polkit_is_authorization_relevant() only
 * that it avoids IPC overhead on the 2nd and subsequent calls when
 * checking authorizations scoped for a session.
 *
 * Returns: #TRUE if the authorization still applies, #FALSE if an
 * error occurred (then error will be set) or if the entity the
 * authorization refers to has gone out of scope.
 *
 * This function is in <literal>libpolkit-dbus</literal>.
 *
 * Since: 0.7
 */
polkit_bool_t  
polkit_tracker_is_authorization_relevant (PolKitTracker *pk_tracker, PolKitAuthorization *auth, DBusError *error)
{

        kit_return_val_if_fail (pk_tracker != NULL, FALSE);
        kit_return_val_if_fail (pk_tracker->con != NULL, FALSE);
        kit_return_val_if_fail (! dbus_error_is_set (error), FALSE);

        /* TODO: optimize... in order to do this sanely we need CK's Manager object to export 
         * a method GetAllSessions() - otherwise we'd need to key off every uid. 
         *
         * It's no biggie we don't have this optimization yet.. it's only used by polkit-auth(1)
         * and the GNOME utility for managing authorizations.
         */
        return _polkit_is_authorization_relevant_internal (pk_tracker->con, auth, NULL, error);
}

#ifdef POLKIT_BUILD_TESTS

static polkit_bool_t
_run_test (void)
{
        return TRUE;
}

KitTest _test_polkit_dbus = {
        "polkit_dbus",
        NULL,
        NULL,
        _run_test
};

#endif /* POLKIT_BUILD_TESTS */
