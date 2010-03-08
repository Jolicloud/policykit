/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-error.c : GError error codes from PolicyKit
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
 * SECTION:polkit-error
 * @title: Error reporting
 * @short_description: Representation of recoverable errors.
 *
 * Error codes from PolicyKit.
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

#include "polkit-types.h"
#include "polkit-error.h"
#include "polkit-debug.h"
#include "polkit-test.h"
#include "polkit-private.h"

/**
 * PolKitError:
 *
 * Objects of this class are used for error reporting.
 **/
struct _PolKitError
{
        polkit_bool_t is_static;
        PolKitErrorCode error_code;
        char *error_message;
};

/**
 * polkit_error_is_set:
 * @error: the error
 *
 * Determine if an error set
 *
 * Returns: #TRUE if, and only if, the error is set
 *
 * Since: 0.7
 */
polkit_bool_t
polkit_error_is_set (PolKitError *error)
{
        return error != NULL;
}

static const char *error_names[POLKIT_ERROR_NUM_ERROR_CODES] = {
        "OutOfMemory",
        "PolicyFileInvalid",
        "GeneralError",
        "NotAuthorizedToReadAuthorizationsForOtherUsers",
        "NotAuthorizedToRevokeAuthorizationsFromOtherUsers",
        "NotAuthorizedToGrantAuthorization",
        "AuthorizationAlreadyExists",
        "NotSupported",
        "NotAuthorizedToModifyDefaults",
};

/**
 * polkit_error_get_error_name:
 * @error: the error
 * 
 * Get the CamelCase name for the error;
 * e.g. #POLKIT_ERROR_OUT_OF_MEMORY maps to "OutOfMemory" and so on.
 *
 * Returns: the string
 *
 * Since: 0.7
 */
const char *
polkit_error_get_error_name (PolKitError *error)
{
        kit_return_val_if_fail (error != NULL, NULL);
        kit_return_val_if_fail (error->error_code >= 0 && error->error_code < POLKIT_ERROR_NUM_ERROR_CODES, NULL);

        return error_names[error->error_code];
}

/**
 * polkit_error_get_error_code:
 * @error: the error object
 * 
 * Returns the error code.
 * 
 * Returns: A value from the #PolKitErrorCode enumeration.
 **/
PolKitErrorCode 
polkit_error_get_error_code (PolKitError *error)
{
        kit_return_val_if_fail (error != NULL, -1);
        return error->error_code;
}

/**
 * polkit_error_get_error_message:
 * @error: the error object
 * 
 * Get the error message.
 * 
 * Returns: A string describing the error. Caller shall not free this string.
 **/
const char *
polkit_error_get_error_message (PolKitError *error)
{
        kit_return_val_if_fail (error != NULL, NULL);
        return error->error_message;
}

/**
 * polkit_error_free:
 * @error: the error
 * 
 * Free an error.
 **/
void
polkit_error_free (PolKitError *error)
{
        kit_return_if_fail (error != NULL);
        if (!error->is_static) {
                kit_free (error->error_message);
                kit_free (error);
        }
}


static PolKitError _oom_error = {TRUE, POLKIT_ERROR_OUT_OF_MEMORY, "Pre-allocated OOM error object"};

/**
 * polkit_error_set_error:
 * @error: the error object
 * @error_code: A value from the #PolKitErrorCode enumeration.
 * @format: printf style formatting string
 * @Varargs: printf style arguments
 * 
 * Sets an error. If OOM, the error will be set to a pre-allocated OOM error.
 *
 * Returns: TRUE if the error was set
 **/
polkit_bool_t
polkit_error_set_error (PolKitError **error, PolKitErrorCode error_code, const char *format, ...)
{
        va_list args;
        PolKitError *e;

        kit_return_val_if_fail (format != NULL, FALSE);

        if (error_code < 0 || error_code >= POLKIT_ERROR_NUM_ERROR_CODES)
                return FALSE;

        if (error == NULL)
                goto out;

        e = kit_new0 (PolKitError, 1);
        if (e == NULL) {
                *error = &_oom_error;
        } else {
                e->is_static = FALSE;
                e->error_code = error_code;
                va_start (args, format);
                e->error_message = kit_strdup_vprintf (format, args);
                va_end (args);
                if (e->error_message == NULL) {
                        kit_free (e);
                        *error = &_oom_error;
                } else {                
                        *error = e;
                }
        }

out:
        return TRUE;
}

#ifdef POLKIT_BUILD_TESTS

static polkit_bool_t
_run_test (void)
{
        unsigned int n;
        PolKitError *e;
        char s[256];

        e = NULL;
        kit_assert (! polkit_error_is_set (e));
        kit_assert (! polkit_error_set_error (&e, -1, "Testing"));
        kit_assert (! polkit_error_set_error (&e, POLKIT_ERROR_NUM_ERROR_CODES, "Testing"));

        for (n = 0; n < POLKIT_ERROR_NUM_ERROR_CODES; n++) {
                polkit_error_set_error (&e, n, "Testing error code %d", n);
                kit_assert (polkit_error_is_set (e));
                kit_assert (polkit_error_get_error_code (e) == n || polkit_error_get_error_code (e) == POLKIT_ERROR_OUT_OF_MEMORY);
                kit_assert (strcmp (polkit_error_get_error_name (e), error_names[polkit_error_get_error_code (e)]) == 0);

                if (polkit_error_get_error_code (e) != POLKIT_ERROR_OUT_OF_MEMORY) {
                        snprintf (s, sizeof (s), "Testing error code %d", n);
                        kit_assert (strcmp (polkit_error_get_error_message (e), s) == 0);
                }

                polkit_error_free (e);
        }

        kit_assert (polkit_error_set_error (NULL, POLKIT_ERROR_OUT_OF_MEMORY, "This error will never get set"));

        return TRUE;
}


KitTest _test_error = {
        "polkit_error",
        NULL,
        NULL,
        _run_test
};

#endif /* POLKIT_BUILD_TESTS */
