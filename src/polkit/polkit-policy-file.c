/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-policy-file.c : policy files
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
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>

#include <expat.h>

#include "polkit-error.h"
#include "polkit-result.h"
#include "polkit-policy-file.h"
#include "polkit-policy-file-entry.h"
#include "polkit-debug.h"
#include "polkit-private.h"
#include "polkit-test.h"

/**
 * SECTION:polkit-policy-file
 * @title: Policy Definition Files
 * @short_description: Represents a set of declared actions.
 *
 * This class is used to represent a policy file.
 **/

/**
 * PolKitPolicyFile:
 *
 * Objects of this class are used to record information about a
 * policy file.
 **/
struct _PolKitPolicyFile
{
        int refcount;
        KitList *entries;
};

enum {
        STATE_NONE,
        STATE_UNKNOWN_TAG,
        STATE_IN_POLICY_CONFIG,
        STATE_IN_POLICY_VENDOR,
        STATE_IN_POLICY_VENDOR_URL,
        STATE_IN_POLICY_ICON_NAME,
        STATE_IN_ACTION,
        STATE_IN_ACTION_DESCRIPTION,
        STATE_IN_ACTION_MESSAGE,
        STATE_IN_ACTION_VENDOR,
        STATE_IN_ACTION_VENDOR_URL,
        STATE_IN_ACTION_ICON_NAME,
        STATE_IN_DEFAULTS,
        STATE_IN_DEFAULTS_ALLOW_ANY,
        STATE_IN_DEFAULTS_ALLOW_INACTIVE,
        STATE_IN_DEFAULTS_ALLOW_ACTIVE,
        STATE_IN_ANNOTATE
};

#define PARSER_MAX_DEPTH 32

typedef struct {
        XML_Parser parser;
        int state;
        int state_stack[PARSER_MAX_DEPTH];
        int stack_depth;

        const char *path;

        char *global_vendor;
        char *global_vendor_url;
        char *global_icon_name;

        char *action_id;
        char *vendor;
        char *vendor_url;
        char *icon_name;

        PolKitResult defaults_allow_any;
        PolKitResult defaults_allow_inactive;
        PolKitResult defaults_allow_active;
        
        PolKitPolicyFile *pf;

        polkit_bool_t load_descriptions;

        KitHash *policy_descriptions;
        KitHash *policy_messages;

        char *policy_description_nolang;
        char *policy_message_nolang;

        /* the language according to $LANG (e.g. en_US, da_DK, fr, en_CA minus the encoding) */
        char *lang;

        /* the value of xml:lang for the thing we're reading in _cdata() */
        char *elem_lang;

        char *annotate_key;
        KitHash *annotations;

        polkit_bool_t is_oom;
} ParserData;

static void
pd_unref_action_data (ParserData *pd)
{
        kit_free (pd->action_id);
        pd->action_id = NULL;

        kit_free (pd->vendor);
        pd->vendor = NULL;
        kit_free (pd->vendor_url);
        pd->vendor_url = NULL;
        kit_free (pd->icon_name);
        pd->icon_name = NULL;

        kit_free (pd->policy_description_nolang);
        pd->policy_description_nolang = NULL;
        kit_free (pd->policy_message_nolang);
        pd->policy_message_nolang = NULL;
        if (pd->policy_descriptions != NULL) {
                kit_hash_unref (pd->policy_descriptions);
                pd->policy_descriptions = NULL;
        }
        if (pd->policy_messages != NULL) {
                kit_hash_unref (pd->policy_messages);
                pd->policy_messages = NULL;
        }
        kit_free (pd->annotate_key);
        pd->annotate_key = NULL;
        if (pd->annotations != NULL) {
                kit_hash_unref (pd->annotations);
                pd->annotations = NULL;
        }
        kit_free (pd->elem_lang);
        pd->elem_lang = NULL;
}

static void
pd_unref_data (ParserData *pd)
{
        pd_unref_action_data (pd);
        kit_free (pd->lang);
        pd->lang = NULL;

        kit_free (pd->global_vendor);
        pd->global_vendor = NULL;
        kit_free (pd->global_vendor_url);
        pd->global_vendor_url = NULL;
        kit_free (pd->global_icon_name);
        pd->global_icon_name = NULL;
}

static void
_start (void *data, const char *el, const char **attr)
{
        int state;
        int num_attr;
        ParserData *pd = data;

        for (num_attr = 0; attr[num_attr] != NULL; num_attr++)
                ;

        state = STATE_NONE;

        switch (pd->state) {
        case STATE_NONE:
                if (strcmp (el, "policyconfig") == 0) {
                        state = STATE_IN_POLICY_CONFIG;
                }
                break;
        case STATE_IN_POLICY_CONFIG:
                if (strcmp (el, "action") == 0) {
                        if (num_attr != 2 || strcmp (attr[0], "id") != 0)
                                goto error;
                        state = STATE_IN_ACTION;

                        if (!polkit_action_validate_id (attr[1]))
                                goto error;

                        pd_unref_action_data (pd);
                        pd->action_id = kit_strdup (attr[1]);
                        if (pd->action_id == NULL)
                                goto oom;
                        pd->policy_descriptions = kit_hash_new (kit_hash_str_hash_func, 
                                                                kit_hash_str_equal_func, 
                                                                kit_hash_str_copy, kit_hash_str_copy,
                                                                kit_free, kit_free);
                        pd->policy_messages = kit_hash_new (kit_hash_str_hash_func, 
                                                            kit_hash_str_equal_func, 
                                                            kit_hash_str_copy, kit_hash_str_copy,
                                                            kit_free, kit_free);

                        /* initialize defaults */
                        pd->defaults_allow_any = POLKIT_RESULT_NO;
                        pd->defaults_allow_inactive = POLKIT_RESULT_NO;
                        pd->defaults_allow_active = POLKIT_RESULT_NO;
                } else if (strcmp (el, "vendor") == 0 && num_attr == 0) {
                        state = STATE_IN_POLICY_VENDOR;
                } else if (strcmp (el, "vendor_url") == 0 && num_attr == 0) {
                        state = STATE_IN_POLICY_VENDOR_URL;
                } else if (strcmp (el, "icon_name") == 0 && num_attr == 0) {
                        state = STATE_IN_POLICY_ICON_NAME;
                }
                break;
        case STATE_IN_ACTION:
                if (strcmp (el, "defaults") == 0) {
                        state = STATE_IN_DEFAULTS;
                } else if (strcmp (el, "description") == 0) {
                        if (num_attr == 2 && strcmp (attr[0], "xml:lang") == 0) {
                                pd->elem_lang = kit_strdup (attr[1]);
                                if (pd->elem_lang == NULL)
                                        goto oom;
                        }
                        state = STATE_IN_ACTION_DESCRIPTION;
                } else if (strcmp (el, "message") == 0) {
                        if (num_attr == 2 && strcmp (attr[0], "xml:lang") == 0) {
                                pd->elem_lang = kit_strdup (attr[1]);
                                if (pd->elem_lang == NULL)
                                        goto oom;
                        }
                        state = STATE_IN_ACTION_MESSAGE;
                } else if (strcmp (el, "vendor") == 0 && num_attr == 0) {
                        state = STATE_IN_ACTION_VENDOR;
                } else if (strcmp (el, "vendor_url") == 0 && num_attr == 0) {
                        state = STATE_IN_ACTION_VENDOR_URL;
                } else if (strcmp (el, "icon_name") == 0 && num_attr == 0) {
                        state = STATE_IN_ACTION_ICON_NAME;
                } else if (strcmp (el, "annotate") == 0) {
                        if (num_attr != 2 || strcmp (attr[0], "key") != 0)
                                goto error;
                        state = STATE_IN_ANNOTATE;

                        kit_free (pd->annotate_key);
                        pd->annotate_key = kit_strdup (attr[1]);
                        if (pd->annotate_key == NULL)
                                goto oom;
                }
                break;
        case STATE_IN_DEFAULTS:
                if (strcmp (el, "allow_any") == 0)
                        state = STATE_IN_DEFAULTS_ALLOW_ANY;
                else if (strcmp (el, "allow_inactive") == 0)
                        state = STATE_IN_DEFAULTS_ALLOW_INACTIVE;
                else if (strcmp (el, "allow_active") == 0)
                        state = STATE_IN_DEFAULTS_ALLOW_ACTIVE;
                break;
        default:
                break;
        }

        if (state == STATE_NONE) {
                //kit_warning ("skipping unknown tag <%s> at line %d of %s", 
                //             el, (int) XML_GetCurrentLineNumber (pd->parser), pd->path);
                state = STATE_UNKNOWN_TAG;
        }

        pd->state = state;
        pd->state_stack[pd->stack_depth] = pd->state;
        pd->stack_depth++;
        return;
oom:
        pd->is_oom = TRUE;
error:
        XML_StopParser (pd->parser, FALSE);
}

static polkit_bool_t
_validate_icon_name (const char *icon_name)
{
        unsigned int n;
        polkit_bool_t ret;
        size_t len;

        ret = FALSE;

        len = strlen (icon_name);

        /* check for common suffixes */
        if (kit_str_has_suffix (icon_name, ".png"))
                goto out;
        if (kit_str_has_suffix (icon_name, ".jpg"))
                goto out;

        /* icon name cannot be a path */
        for (n = 0; n < len; n++) {
                if (icon_name [n] == '/') {
                        goto out;
                }
        }

        ret = TRUE;

out:
        return ret;
}

static void
_cdata (void *data, const char *s, int len)
{
        char *str;
        ParserData *pd = data;

        str = kit_strndup (s, len);
        if (str == NULL)
                goto oom;

        switch (pd->state) {

        case STATE_IN_ACTION_DESCRIPTION:
                if (pd->load_descriptions) {
                        if (pd->elem_lang == NULL) {
                                kit_free (pd->policy_description_nolang);
                                pd->policy_description_nolang = str;
                                str = NULL;
                        } else {
                                if (!kit_hash_insert (pd->policy_descriptions, pd->elem_lang, str))
                                        goto oom;
                        }
                }
                break;

        case STATE_IN_ACTION_MESSAGE:
                if (pd->load_descriptions) {
                        if (pd->elem_lang == NULL) {
                                kit_free (pd->policy_message_nolang);
                                pd->policy_message_nolang = str;
                                str = NULL;
                        } else {
                                if (!kit_hash_insert (pd->policy_messages, pd->elem_lang, str))
                                        goto oom;
                        }
                }
                break;

        case STATE_IN_POLICY_VENDOR:
                if (pd->load_descriptions) {
                        kit_free (pd->global_vendor);
                        pd->global_vendor = str;
                        str = NULL;
                }
                break;

        case STATE_IN_POLICY_VENDOR_URL:
                if (pd->load_descriptions) {
                        kit_free (pd->global_vendor_url);
                        pd->global_vendor_url = str;
                        str = NULL;
                }
                break;

        case STATE_IN_POLICY_ICON_NAME:
                if (! _validate_icon_name (str)) {
                        kit_warning ("Icon name '%s' is invalid", str);
                        goto error;
                }

                if (pd->load_descriptions) {
                        kit_free (pd->global_icon_name);
                        pd->global_icon_name = str;
                        str = NULL;
                }
                break;

        case STATE_IN_ACTION_VENDOR:
                if (pd->load_descriptions) {
                        kit_free (pd->vendor);
                        pd->vendor = str;
                        str = NULL;
                }
                break;

        case STATE_IN_ACTION_VENDOR_URL:
                if (pd->load_descriptions) {
                        kit_free (pd->vendor_url);
                        pd->vendor_url = str;
                        str = NULL;
                }
                break;

        case STATE_IN_ACTION_ICON_NAME:
                if (! _validate_icon_name (str)) {
                        kit_warning ("Icon name '%s' is invalid", str);
                        goto error;
                }

                if (pd->load_descriptions) {
                        kit_free (pd->icon_name);
                        pd->icon_name = str;
                        str = NULL;
                }
                break;

        case STATE_IN_DEFAULTS_ALLOW_ANY:
                if (!polkit_result_from_string_representation (str, &pd->defaults_allow_any))
                        goto error;
                break;
        case STATE_IN_DEFAULTS_ALLOW_INACTIVE:
                if (!polkit_result_from_string_representation (str, &pd->defaults_allow_inactive))
                        goto error;
                break;
        case STATE_IN_DEFAULTS_ALLOW_ACTIVE:
                if (!polkit_result_from_string_representation (str, &pd->defaults_allow_active))
                        goto error;
                break;

        case STATE_IN_ANNOTATE:
                if (pd->annotations == NULL) {
                        pd->annotations = kit_hash_new (kit_hash_str_hash_func, 
                                                        kit_hash_str_equal_func, 
                                                        kit_hash_str_copy, kit_hash_str_copy,
                                                        kit_free, kit_free);
                        if (pd->annotations == NULL)
                                goto oom;
                }
                if (!kit_hash_insert (pd->annotations, pd->annotate_key, str))
                        goto oom;
                break;

        default:
                break;
        }
        kit_free (str);
        return;
oom:
        pd->is_oom = TRUE;
error:
        kit_free (str);
        XML_StopParser (pd->parser, FALSE);
}

/**
 * _localize:
 * @translations: a mapping from xml:lang to the value, e.g. 'da' -> 'Smadre', 'en_CA' -> 'Punch, Aye!'
 * @untranslated: the untranslated value, e.g. 'Punch'
 * @lang: the locale we're interested in, e.g. 'da_DK', 'da', 'en_CA', 'en_US'; basically just $LANG
 * with the encoding cut off. Maybe be NULL.
 *
 * Pick the correct translation to use.
 *
 * Returns: the localized string to use
 */
static const char *
_localize (KitHash *translations, const char *untranslated, const char *lang)
{
        const char *result;
        char lang2[256];
        int n;

        if (lang == NULL) {
                result = untranslated;
                goto out;
        }

        /* first see if we have the translation */
        result = (const char *) kit_hash_lookup (translations, (void *) lang, NULL);
        if (result != NULL)
                goto out;

        /* we could have a translation for 'da' but lang=='da_DK'; cut off the last part and try again */
        strncpy (lang2, lang, sizeof (lang2));
        for (n = 0; lang2[n] != '\0'; n++) {
                if (lang2[n] == '_') {
                        lang2[n] = '\0';
                        break;
                }
        }
        result = (const char *) kit_hash_lookup (translations, (void *) lang2, NULL);
        if (result != NULL)
                goto out;

        /* fall back to untranslated */
        result = untranslated;
out:
        return result;
}

static void
_end (void *data, const char *el)
{
        ParserData *pd = data;
        KitList *l;

        kit_free (pd->elem_lang);
        pd->elem_lang = NULL;

        switch (pd->state) {
        case STATE_IN_ACTION:
        {
                const char *policy_description;
                const char *policy_message;
                PolKitPolicyFileEntry *pfe;
                char *vendor;
                char *vendor_url;
                char *icon_name;

                vendor = pd->vendor;
                if (vendor == NULL)
                        vendor = pd->global_vendor;

                vendor_url = pd->vendor_url;
                if (vendor_url == NULL)
                        vendor_url = pd->global_vendor_url;

                icon_name = pd->icon_name;
                if (icon_name == NULL)
                        icon_name = pd->global_icon_name;

                /* NOTE: caller takes ownership of the annotations object */
                pfe = _polkit_policy_file_entry_new (pd->action_id, 
                                                     vendor,
                                                     vendor_url,
                                                     icon_name,
                                                     pd->defaults_allow_any,
                                                     pd->defaults_allow_inactive,
                                                     pd->defaults_allow_active,
                                                     pd->annotations);
                if (pfe == NULL)
                        goto oom;
                pd->annotations = NULL;

                if (pd->load_descriptions) {
                        policy_description = _localize (pd->policy_descriptions, pd->policy_description_nolang, pd->lang);
                        policy_message = _localize (pd->policy_messages, pd->policy_message_nolang, pd->lang);
                } else {
                        policy_description = NULL;
                        policy_message = NULL;
                }

                if (pd->load_descriptions) {
                        if (!_polkit_policy_file_entry_set_descriptions (pfe,
                                                                         policy_description,
                                                                         policy_message)) {
                                polkit_policy_file_entry_unref (pfe);
                                goto oom;
                        }
                }

                l = kit_list_prepend (pd->pf->entries, pfe);
                if (l == NULL) {
                        polkit_policy_file_entry_unref (pfe);
                        goto oom;
                }
                pd->pf->entries = l;
                break;
        }
        default:
                break;
        }

        --pd->stack_depth;
        if (pd->stack_depth < 0 || pd->stack_depth >= PARSER_MAX_DEPTH) {
                polkit_debug ("reached max depth?");
                goto error;
        }
        if (pd->stack_depth > 0)
                pd->state = pd->state_stack[pd->stack_depth - 1];
        else
                pd->state = STATE_NONE;

        return;
oom:
        pd->is_oom = 1;
error:
        XML_StopParser (pd->parser, FALSE);
}

/**
 * polkit_policy_file_new:
 * @path: path to file
 * @load_descriptions: whether descriptions should be loaded
 * @error: Return location for error
 * 
 * Load a policy file.
 * 
 * Returns: The new object or #NULL if error is set
 **/
PolKitPolicyFile *
polkit_policy_file_new (const char *path, polkit_bool_t load_descriptions, PolKitError **error)
{
        PolKitPolicyFile *pf;
        ParserData pd;
        int xml_res;
        char *lang;
	char *buf;
	size_t buflen;

        pf = NULL;
        buf = NULL;

        /* clear parser data */
        memset (&pd, 0, sizeof (ParserData));

        if (!kit_str_has_suffix (path, ".policy")) {
                polkit_error_set_error (error, 
                                        POLKIT_ERROR_POLICY_FILE_INVALID,
                                        "Policy files must have extension .policy; file '%s' doesn't", path);
                goto error;
        }

	if (!kit_file_get_contents (path, &buf, &buflen)) {
                if (errno == ENOMEM) {
                        polkit_error_set_error (error, POLKIT_ERROR_OUT_OF_MEMORY,
                                                "Cannot load PolicyKit policy file at '%s': %s",
                                                path,
                                                "No memory for parser");
                } else {
                        polkit_error_set_error (error, POLKIT_ERROR_POLICY_FILE_INVALID,
                                                "Cannot load PolicyKit policy file at '%s': %m",
                                                path);
                }
		goto error;
        }

        pd.path = path;
/* #ifdef POLKIT_BUILD_TESTS
   TODO: expat appears to leak on certain OOM paths
*/
#if 0
        XML_Memory_Handling_Suite memsuite = {p_malloc, p_realloc, kit_free};
        pd.parser = XML_ParserCreate_MM (NULL, &memsuite, NULL);
#else
        pd.parser = XML_ParserCreate (NULL);
#endif
        pd.stack_depth = 0;
        if (pd.parser == NULL) {
                polkit_error_set_error (error, POLKIT_ERROR_OUT_OF_MEMORY,
                                        "Cannot load PolicyKit policy file at '%s': %s",
                                        path,
                                        "No memory for parser");
                goto error;
        }
	XML_SetUserData (pd.parser, &pd);
	XML_SetElementHandler (pd.parser, _start, _end);
	XML_SetCharacterDataHandler (pd.parser, _cdata);

        pf = kit_new0 (PolKitPolicyFile, 1);
        if (pf == NULL) {
                polkit_error_set_error (error, POLKIT_ERROR_OUT_OF_MEMORY,
                                        "Cannot load PolicyKit policy file at '%s': No memory for object",
                                        path);
                goto error;
        }

        pf->refcount = 1;

        /* init parser data */
        pd.state = STATE_NONE;
        pd.pf = pf;
        pd.load_descriptions = load_descriptions;
        lang = getenv ("LANG");
        if (lang != NULL) {
                int n;
                pd.lang = kit_strdup (lang);
                if (pd.lang == NULL) {
                        polkit_error_set_error (error, POLKIT_ERROR_OUT_OF_MEMORY,
                                                "Cannot load PolicyKit policy file at '%s': No memory for lang",
                                                path);
                        goto error;
                }
                for (n = 0; pd.lang[n] != '\0'; n++) {
                        if (pd.lang[n] == '.') {
                                pd.lang[n] = '\0';
                                break;
                        }
                }
        }

        xml_res = XML_Parse (pd.parser, buf, buflen, 1);

	if (xml_res == 0) {
                if (XML_GetErrorCode (pd.parser) == XML_ERROR_NO_MEMORY) {
                        polkit_error_set_error (error, POLKIT_ERROR_OUT_OF_MEMORY,
                                                "Out of memory parsing %s",
                                                path);
                } else if (pd.is_oom) {
                        polkit_error_set_error (error, POLKIT_ERROR_OUT_OF_MEMORY,
                                                "Out of memory parsing %s",
                                                path);
                } else {
                        polkit_error_set_error (error, POLKIT_ERROR_POLICY_FILE_INVALID,
                                                "%s:%d: parse error: %s",
                                                path, 
                                                (int) XML_GetCurrentLineNumber (pd.parser),
                                                XML_ErrorString (XML_GetErrorCode (pd.parser)));
                }
		XML_ParserFree (pd.parser);
		goto error;
	}

	XML_ParserFree (pd.parser);
	kit_free (buf);
        pd_unref_data (&pd);
        return pf;
error:
        if (pf != NULL)
                polkit_policy_file_unref (pf);
        pd_unref_data (&pd);
        kit_free (buf);
        return NULL;
}

/**
 * polkit_policy_file_ref:
 * @policy_file: the policy file object
 * 
 * Increase reference count.
 * 
 * Returns: the object
 **/
PolKitPolicyFile *
polkit_policy_file_ref (PolKitPolicyFile *policy_file)
{
        kit_return_val_if_fail (policy_file != NULL, policy_file);
        policy_file->refcount++;
        return policy_file;
}

/**
 * polkit_policy_file_unref:
 * @policy_file: the policy file object
 * 
 * Decreases the reference count of the object. If it becomes zero,
 * the object is freed. Before freeing, reference counts on embedded
 * objects are decresed by one.
 **/
void
polkit_policy_file_unref (PolKitPolicyFile *policy_file)
{
        KitList *i;
        kit_return_if_fail (policy_file != NULL);
        policy_file->refcount--;
        if (policy_file->refcount > 0) 
                return;
        for (i = policy_file->entries; i != NULL; i = i->next) {
                polkit_policy_file_entry_unref (i->data);
        }
        if (policy_file->entries != NULL)
                kit_list_free (policy_file->entries);
        kit_free (policy_file);
}

/**
 * polkit_policy_file_entry_foreach:
 * @policy_file: the policy file object
 * @cb: callback to invoke for each entry
 * @user_data: user data
 * 
 * Visits all entries in a policy file.
 *
 * Returns: #TRUE if the iteration was short-circuited
 **/
polkit_bool_t
polkit_policy_file_entry_foreach (PolKitPolicyFile                 *policy_file,
                                  PolKitPolicyFileEntryForeachFunc  cb,
                                  void                              *user_data)
{
        KitList *i;

        kit_return_val_if_fail (policy_file != NULL, FALSE);
        kit_return_val_if_fail (cb != NULL, FALSE);

        for (i = policy_file->entries; i != NULL; i = i->next) {
                PolKitPolicyFileEntry *pfe = i->data;
                if (cb (policy_file, pfe, user_data))
                        return TRUE;
        }

        return FALSE;
}
#ifdef POLKIT_BUILD_TESTS

/* this checks that the policy descriptions read from test-valid-3-lang.policy are correct */
static polkit_bool_t
_check_pf (PolKitPolicyFile *pf, PolKitPolicyFileEntry *pfe, void *user_data)
{
        const char *r_msg;
        const char *r_desc;
        char *msg;
        char *desc;
        char *lang;
        int *counter = (int *) user_data;
        polkit_bool_t is_danish;

        is_danish = FALSE;
        lang = getenv ("LANG");
        if (lang != NULL) {
                if (strcmp (lang, "da_DK.UTF8") == 0 ||
                    strcmp (lang, "da_DK") == 0 ||
                    strcmp (lang, "da") == 0)
                        is_danish = TRUE;
        }
        

        if (strcmp (polkit_policy_file_entry_get_id (pfe), "org.example.valid3") == 0) {
                if (is_danish) {
                        desc = "example (danish)";
                        msg = "message (danish)";
                } else {
                        desc = "example";
                        msg = "message";
                }
                r_desc = polkit_policy_file_entry_get_action_description (pfe);
                r_msg = polkit_policy_file_entry_get_action_message (pfe);

                if (strcmp (r_desc, desc) == 0 &&
                    strcmp (r_msg, msg) == 0) 
                        *counter += 1;

        }  else if (strcmp (polkit_policy_file_entry_get_id (pfe), "org.example.valid3b") == 0) {
                if (is_danish) {
                        desc = "example 2 (danish)";
                        msg = "message 2 (danish)";
                } else {
                        desc = "example 2";
                        msg = "message 2";
                }
                r_desc = polkit_policy_file_entry_get_action_description (pfe);
                r_msg = polkit_policy_file_entry_get_action_message (pfe);

                if (strcmp (r_desc, desc) == 0 &&
                    strcmp (r_msg, msg) == 0) 
                        *counter += 1;
        }

        return FALSE;
}

static polkit_bool_t
_run_test (void)
{
        int m;
        unsigned int n;
        PolKitPolicyFile *pf;
        PolKitError *error;
        const char *valid_files[] = {
                TEST_DATA_DIR "valid/test-valid-1.policy",
                TEST_DATA_DIR "valid/test-valid-2-annotations.policy",
                TEST_DATA_DIR "valid/test-valid-3-lang.policy",
                TEST_DATA_DIR "valid/test-valid-4-unknown-tags.policy",
        };
        const char *invalid_files[] = {
                TEST_DATA_DIR "invalid/non-existant-file.policy",
                TEST_DATA_DIR "invalid/bad.extension",
                TEST_DATA_DIR "invalid/test-invalid-1-action-id.policy",
                TEST_DATA_DIR "invalid/test-invalid-2-bogus-any.policy",
                TEST_DATA_DIR "invalid/test-invalid-3-bogus-inactive.policy",
                TEST_DATA_DIR "invalid/test-invalid-4-bogus-active.policy",
                TEST_DATA_DIR "invalid/test-invalid-5-max-depth.policy",
        };

        for (n = 0; n < sizeof (invalid_files) / sizeof (char*); n++) {
                error = NULL;
                kit_assert (polkit_policy_file_new (invalid_files[n], TRUE, &error) == NULL);
                kit_assert (polkit_error_get_error_code (error) == POLKIT_ERROR_OUT_OF_MEMORY ||
                          polkit_error_get_error_code (error) == POLKIT_ERROR_POLICY_FILE_INVALID);
                polkit_error_free (error);
        }
        
        for (n = 0; n < sizeof (valid_files) / sizeof (char*); n++) {

                for (m = 0; m < 6; m++) {
                        polkit_bool_t load_descriptions;

                        /* only run the multiple lang tests for test-valid-3-lang.policy */
                        if (n != 2) {
                                if (m > 0)
                                        break;
                        }

                        load_descriptions = TRUE;
                        
                        switch (m) {
                        case 0:
                                unsetenv ("LANG");
                                break;
                        case 1:
                                setenv ("LANG", "da_DK.UTF8", 1);
                                break;
                        case 2:
                                setenv ("LANG", "da_DK", 1);
                                break;
                        case 3:
                                setenv ("LANG", "da", 1);
                                break;
                        case 4:
                                setenv ("LANG", "en_CA", 1);
                                break;
                        case 5:
                                unsetenv ("LANG");
                                load_descriptions = FALSE;
                                break;
                        }

                        error = NULL;
                        if ((pf = polkit_policy_file_new (valid_files[n], load_descriptions, &error)) == NULL) {
                                kit_assert (polkit_error_get_error_code (error) == POLKIT_ERROR_OUT_OF_MEMORY);
                                polkit_error_free (error);
                        } else {

                                if (n == 2 && m != 5) {
                                        int num_passed;

                                        num_passed = 0;
                                        polkit_policy_file_entry_foreach (pf,
                                                                          _check_pf,
                                                                          &num_passed);
                                        kit_assert (num_passed == 2);
                                }

                                polkit_policy_file_ref (pf);
                                polkit_policy_file_unref (pf);
                                polkit_policy_file_unref (pf);
                        }
                }
        }

        return TRUE;
}

KitTest _test_policy_file = {
        "polkit_policy_file",
        NULL,
        NULL,
        _run_test
};

#endif /* POLKIT_BUILD_TESTS */
