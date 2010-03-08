/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-result.c : result codes from PolicyKit
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
 * SECTION:polkit-result
 * @title: Results
 * @short_description: Definition of results of PolicyKit queries.
 *
 * These functions are used to manipulate PolicyKit results.
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

#include "polkit-result.h"
#include "polkit-test.h"
#include "polkit-private.h"


static const struct {
        PolKitResult result;
        const char *str;
} mapping[POLKIT_RESULT_N_RESULTS] = 
{
        {POLKIT_RESULT_UNKNOWN, "unknown"},
        {POLKIT_RESULT_NO, "no"},
        {POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH, "auth_admin"},
        {POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION, "auth_admin_keep_session"},
        {POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS, "auth_admin_keep_always"},
        {POLKIT_RESULT_ONLY_VIA_SELF_AUTH, "auth_self"},
        {POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION, "auth_self_keep_session"},
        {POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS, "auth_self_keep_always"},
        {POLKIT_RESULT_YES, "yes"},
        {POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT, "auth_admin_one_shot"},
        {POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT, "auth_self_one_shot"},
};


/**
 * polkit_result_to_string_representation:
 * @result: the given result to get a textual representation of
 * 
 * Gives a textual representation of a #PolKitResult object. This
 * string is not suitable for displaying to an end user (it's not
 * localized for starters) but is useful for serialization as it can
 * be converted back to a #PolKitResult object using
 * polkit_result_from_string_representation().
 * 
 * Returns: string representing the result (do not free) or #NULL if the given result is invalid
 **/
const char *
polkit_result_to_string_representation (PolKitResult result)
{
        if (result < 0 || result >= POLKIT_RESULT_N_RESULTS) {
                kit_warning ("The passed result code, %d, is not valid", result);
                return NULL;
        }

        return mapping[result].str;
}

/**
 * polkit_result_from_string_representation:
 * @string: textual representation of a #PolKitResult object
 * @out_result: return location for #PolKitResult
 * 
 * Given a textual representation of a #PolKitResult object, find the
 * #PolKitResult value.
 * 
 * Returns: TRUE if the textual representation was valid, otherwise FALSE
 **/
polkit_bool_t
polkit_result_from_string_representation (const char *string, PolKitResult *out_result)
{
        int n;

        kit_return_val_if_fail (out_result != NULL, FALSE);

        for (n = 0; n < POLKIT_RESULT_N_RESULTS; n++) {
                if (strcmp (mapping[n].str, string) == 0) {
                        *out_result = mapping[n].result;
                        goto found;
                }
        }

        return FALSE;
found:
        return TRUE;
}

#ifdef POLKIT_BUILD_TESTS

static polkit_bool_t
_run_test (void)
{
        PolKitResult n;
        PolKitResult m;

        for (n = 0; n < POLKIT_RESULT_N_RESULTS; n++) {
                kit_assert (polkit_result_from_string_representation (polkit_result_to_string_representation (n), &m) && n== m);
        }

        kit_assert (polkit_result_to_string_representation ((PolKitResult) -1) == NULL);
        kit_assert (polkit_result_to_string_representation (POLKIT_RESULT_N_RESULTS) == NULL);

        kit_assert (! polkit_result_from_string_representation ("non-exiting-result-id", &m));


        return TRUE;
}

KitTest _test_result = {
        "polkit_result",
        NULL,
        NULL,
        _run_test
};

#endif /* POLKIT_BUILD_TESTS */
