/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-result.h : result codes from PolicyKit
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

#if !defined (POLKIT_COMPILATION) && !defined(_POLKIT_INSIDE_POLKIT_H)
#error "Only <polkit/polkit.h> can be included directly, this file may disappear or change contents."
#endif

#ifndef POLKIT_RESULT_H
#define POLKIT_RESULT_H

#include <polkit/polkit-types.h>

POLKIT_BEGIN_DECLS

/**
 * PolKitResult:
 * @POLKIT_RESULT_UNKNOWN: The result is unknown / cannot be
 * computed. This is mostly used internally in libpolkit.
 * @POLKIT_RESULT_NO: Access denied.
 * @POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT: Access denied, but
 * authentication by the caller as administrator (e.g. root or a
 * member in the wheel group depending on configuration) will grant
 * access exactly one time to the process the caller is originating
 * from. See polkit_context_is_caller_authorized() for discussion (and
 * limitations) about one-shot authorizations.
 * @POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH: Access denied, but
 * authentication by the caller as administrator (e.g. root or a
 * member in the wheel group depending on configuration) will grant
 * access to the process the caller is originating from.
 * @POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION: Access denied, but
 * authentication by the caller as administrator (e.g. root or a
 * member in the wheel group depending on configuration) will grant
 * access for the remainder of the session
 * @POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS: Access denied, but
 * authentication by the caller as administrator (e.g. root or a
 * member in the wheel group depending on configuration) will grant
 * access in the future.
 * @POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT: Access denied, but
 * authentication by the caller as himself will grant access exactly
 * one time to the process the caller is originating from. See
 * polkit_context_is_caller_authorized() for discussion (and
 * limitations) about one-shot authorizations.
 * @POLKIT_RESULT_ONLY_VIA_SELF_AUTH: Access denied, but
 * authentication by the caller as himself will grant access to the
 * process the caller is originating from.
 * @POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION: Access denied, but
 * authentication by the caller as himself will grant access to the
 * resource for the remainder of the session
 * @POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS: Access denied, but
 * authentication by the caller as himself will grant access to the
 * resource in the future.
 * @POLKIT_RESULT_YES: Access granted.
 * @POLKIT_RESULT_N_RESULTS: Number of result codes
 *
 * Result codes from queries to PolicyKit. This enumeration may grow
 * in the future. One should never rely on the ordering
 */
typedef enum
{
        POLKIT_RESULT_UNKNOWN,

        POLKIT_RESULT_NO,

        POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH,
        POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION,
        POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS,

        POLKIT_RESULT_ONLY_VIA_SELF_AUTH,
        POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION,
        POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS,

        POLKIT_RESULT_YES,

        POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT,
        POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT,

        POLKIT_RESULT_N_RESULTS
} PolKitResult;

const char *
polkit_result_to_string_representation (PolKitResult result);

polkit_bool_t
polkit_result_from_string_representation (const char *string, PolKitResult *out_result);

POLKIT_END_DECLS

#endif /* POLKIT_RESULT_H */
