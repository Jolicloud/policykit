/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-config.h : Configuration file
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
#include <regex.h>
#include <syslog.h>
#include <regex.h>

#include <expat.h>

#include "polkit-config.h"
#include "polkit-debug.h"
#include "polkit-error.h"
#include "polkit-private.h"
#include "polkit-test.h"

/**
 * SECTION:polkit-config
 * @title: Configuration
 * @short_description: Represents the system-wide <literal>/etc/PolicyKit/PolicyKit.conf</literal> file.
 *
 * This class is used to represent the /etc/PolicyKit/PolicyKit.conf
 * configuration file. Applications using PolicyKit should never use
 * this class; it's only here for integration with other PolicyKit
 * components.
 **/

enum {
        STATE_NONE,
        STATE_UNKNOWN_TAG,
        STATE_IN_CONFIG,
        STATE_IN_MATCH,
        STATE_IN_RETURN,
        STATE_IN_DEFINE_ADMIN_AUTH,
};

struct ConfigNode;
typedef struct ConfigNode ConfigNode;

/**
 * PolKitConfig:
 *
 * This class represents the system-wide configuration file for
 * PolicyKit. Applications using PolicyKit should never use this
 * class; it's only here for integration with other PolicyKit
 * components.
 **/
struct _PolKitConfig
{
        int refcount;
        ConfigNode *top_config_node;
};

#define PARSER_MAX_DEPTH 32

typedef struct {
        XML_Parser parser;
        int state;
        PolKitConfig *pk_config;
        const char *path;

        int state_stack[PARSER_MAX_DEPTH];
        ConfigNode *node_stack[PARSER_MAX_DEPTH];

        int stack_depth;
} ParserData;

enum {
        NODE_TYPE_NOP,
        NODE_TYPE_TOP,
        NODE_TYPE_MATCH,
        NODE_TYPE_RETURN,
        NODE_TYPE_DEFINE_ADMIN_AUTH,
};

enum {
        MATCH_TYPE_ACTION,
        MATCH_TYPE_USER,
};

static const char * const match_names[] = 
{
        "action",
        "user",
};

static const char * const define_admin_auth_names[] = 
{
        "user",
        "group",
};

struct ConfigNode
{
        int node_type;

        union {

                struct {
                        int match_type;
                        char *data;
                        regex_t preq;
                } node_match;

                struct {
                        PolKitResult result;
                } node_return;

                struct {
                        PolKitConfigAdminAuthType admin_type;
                        char *data;
                } node_define_admin_auth;

        } data;

        KitList *children;
};


static ConfigNode *
config_node_new (void)
{
        ConfigNode *node;
        node = kit_new0 (ConfigNode, 1);
        return node;
}

static void
config_node_dump_real (ConfigNode *node, unsigned int indent)
{
        KitList *i;
        unsigned int n;
        char buf[128];

        for (n = 0; n < indent && n < sizeof (buf) - 1; n++)
                buf[n] = ' ';
        buf[n] = '\0';
        
        switch (node->node_type) {
        case NODE_TYPE_NOP:
                polkit_debug ("%sNOP", buf);
                break;
        case NODE_TYPE_TOP:
                polkit_debug ("%sTOP", buf);
                break;
        case NODE_TYPE_MATCH:
                polkit_debug ("%sMATCH %s (%d) with '%s'", 
                           buf, 
                           match_names[node->data.node_match.match_type],
                           node->data.node_match.match_type,
                           node->data.node_match.data);
                break;
        case NODE_TYPE_RETURN:
                polkit_debug ("%sRETURN %s (%d)",
                           buf,
                           polkit_result_to_string_representation (node->data.node_return.result),
                           node->data.node_return.result);
                break;
        case NODE_TYPE_DEFINE_ADMIN_AUTH:
                polkit_debug ("%sDEFINE_ADMIN_AUTH %s (%d) with '%s'", 
                           buf, 
                           define_admin_auth_names[node->data.node_define_admin_auth.admin_type],
                           node->data.node_define_admin_auth.admin_type,
                           node->data.node_define_admin_auth.data);
                break;
                break;
        }

        for (i = node->children; i != NULL; i = i->next) {
                ConfigNode *child = i->data;
                config_node_dump_real (child, indent + 2);
        }
}

static void
config_node_dump (ConfigNode *node)
{
        
        config_node_dump_real (node, 0);
}

static void
config_node_unref (ConfigNode *node)
{
        KitList *i;

        switch (node->node_type) {
        case NODE_TYPE_NOP:
                break;
        case NODE_TYPE_TOP:
                break;
        case NODE_TYPE_MATCH:
                kit_free (node->data.node_match.data);
                regfree (&(node->data.node_match.preq));
                break;
        case NODE_TYPE_RETURN:
                break;
        case NODE_TYPE_DEFINE_ADMIN_AUTH:
                kit_free (node->data.node_define_admin_auth.data);
                break;
        }

        for (i = node->children; i != NULL; i = i->next) {
                ConfigNode *child = i->data;
                config_node_unref (child);
        }
        kit_list_free (node->children);
        kit_free (node);
}

static void
_start (void *data, const char *el, const char **attr)
{
        int state;
        int num_attr;
        ParserData *pd = data;
        ConfigNode *node;

        polkit_debug ("_start for node '%s' (at depth=%d)", el, pd->stack_depth);

        for (num_attr = 0; attr[num_attr] != NULL; num_attr++)
                ;

        state = STATE_NONE;
        node = config_node_new ();
        node->node_type = NODE_TYPE_NOP;

        switch (pd->state) {
        case STATE_NONE:
                if (strcmp (el, "config") == 0) {
                        state = STATE_IN_CONFIG;
                        polkit_debug ("parsed config node");

                        if (pd->pk_config->top_config_node != NULL) {
                                polkit_debug ("Multiple config nodes?");
                                goto error;
                        }

                        node->node_type = NODE_TYPE_TOP;
                        pd->pk_config->top_config_node = node;
                }
                break;
        case STATE_IN_CONFIG: /* explicit fallthrough */
        case STATE_IN_MATCH:
                if ((strcmp (el, "match") == 0) && (num_attr == 2)) {

                        node->node_type = NODE_TYPE_MATCH;
                        if (strcmp (attr[0], "action") == 0) {
                                node->data.node_match.match_type = MATCH_TYPE_ACTION;
                        } else if (strcmp (attr[0], "user") == 0) {
                                node->data.node_match.match_type = MATCH_TYPE_USER;
                        } else {
                                polkit_debug ("Unknown match rule '%s'", attr[0]);
                                goto error;
                        }

                        node->data.node_match.data = kit_strdup (attr[1]);
                        if (regcomp (&(node->data.node_match.preq), node->data.node_match.data, REG_NOSUB|REG_EXTENDED) != 0) {
                                polkit_debug ("Invalid expression '%s'", node->data.node_match.data);
                                goto error;
                        }

                        state = STATE_IN_MATCH;
                        polkit_debug ("parsed match node ('%s' (%d) -> '%s')", 
                                   attr[0], 
                                   node->data.node_match.match_type,
                                   node->data.node_match.data);

                } else if ((strcmp (el, "return") == 0) && (num_attr == 2)) {

                        node->node_type = NODE_TYPE_RETURN;

                        if (strcmp (attr[0], "result") == 0) {
                                PolKitResult r;
                                if (!polkit_result_from_string_representation (attr[1], &r)) {
                                        polkit_debug ("Unknown return result '%s'", attr[1]);
                                        goto error;
                                }
                                node->data.node_return.result = r;
                        } else {
                                polkit_debug ("Unknown return rule '%s'", attr[0]);
                                goto error;
                        }

                        state = STATE_IN_RETURN;
                        polkit_debug ("parsed return node ('%s' (%d))",
                                   attr[1],
                                   node->data.node_return.result);
                } else if ((strcmp (el, "define_admin_auth") == 0) && (num_attr == 2)) {

                        node->node_type = NODE_TYPE_DEFINE_ADMIN_AUTH;
                        if (strcmp (attr[0], "user") == 0) {
                                node->data.node_define_admin_auth.admin_type = POLKIT_CONFIG_ADMIN_AUTH_TYPE_USER;
                        } else if (strcmp (attr[0], "group") == 0) {
                                node->data.node_define_admin_auth.admin_type = POLKIT_CONFIG_ADMIN_AUTH_TYPE_GROUP;
                        } else {
                                polkit_debug ("Unknown define_admin_auth rule '%s'", attr[0]);
                                goto error;
                        }

                        node->data.node_define_admin_auth.data = kit_strdup (attr[1]);

                        state = STATE_IN_DEFINE_ADMIN_AUTH;
                        polkit_debug ("parsed define_admin_auth node ('%s' (%d) -> '%s')", 
                                   attr[0], 
                                   node->data.node_define_admin_auth.admin_type,
                                   node->data.node_define_admin_auth.data);


                }
                break;
        }

        if (state == STATE_NONE || node == NULL) {
                kit_warning ("skipping unknown tag <%s> at line %d of %s", 
                             el, (int) XML_GetCurrentLineNumber (pd->parser), pd->path);
                state = STATE_UNKNOWN_TAG;
        }

        if (pd->stack_depth < 0 || pd->stack_depth >= PARSER_MAX_DEPTH) {
                polkit_debug ("reached max depth?");
                goto error;
        }
        pd->state = state;
        pd->state_stack[pd->stack_depth] = pd->state;
        pd->node_stack[pd->stack_depth] = node;

        if (pd->stack_depth > 0) {
                pd->node_stack[pd->stack_depth - 1]->children = 
                        kit_list_append (pd->node_stack[pd->stack_depth - 1]->children, node);
        }

        pd->stack_depth++;
        polkit_debug ("now in state=%d (after _start, depth=%d)", pd->state, pd->stack_depth);
        return;

error:
        if (node != NULL) {
                config_node_unref (node);
        }
        XML_StopParser (pd->parser, FALSE);
}

static void
_cdata (void *data, const char *s, int len)
{
}

static void
_end (void *data, const char *el)
{
        ParserData *pd = data;

        polkit_debug ("_end for node '%s' (at depth=%d)", el, pd->stack_depth);

        --pd->stack_depth;
        if (pd->stack_depth < 0 || pd->stack_depth >= PARSER_MAX_DEPTH) {
                polkit_debug ("reached max depth?");
                goto error;
        }
        if (pd->stack_depth > 0)
                pd->state = pd->state_stack[pd->stack_depth - 1];
        else
                pd->state = STATE_NONE;
        polkit_debug ("now in state=%d (after _end, depth=%d)", pd->state, pd->stack_depth);
        return;
error:
        XML_StopParser (pd->parser, FALSE);
}

/**
 * polkit_config_new:
 * @path: Path to configuration, typically /etc/PolicyKit/PolicyKit.conf is passed.
 * @error: return location for error
 * 
 * Load and parse a PolicyKit configuration file.
 * 
 * Returns: the configuration file object
 **/
PolKitConfig *
polkit_config_new (const char *path, PolKitError **error)
{
        ParserData pd;
        int xml_res;
        PolKitConfig *pk_config;
	char *buf;
	size_t buflen;

        /* load and parse the configuration file */
        pk_config = NULL;

	if (!kit_file_get_contents (path, &buf, &buflen)) {
                polkit_error_set_error (error, POLKIT_ERROR_POLICY_FILE_INVALID,
                                        "Cannot load PolicyKit policy file at '%s': %m",
                                        path);
		goto error;
        }

        pd.parser = XML_ParserCreate (NULL);
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

        pk_config = kit_new0 (PolKitConfig, 1);
        pk_config->refcount = 1;

        pd.state = STATE_NONE;
        pd.pk_config = pk_config;
        pd.node_stack[0] = NULL;
        pd.stack_depth = 0;
        pd.path = path;

        xml_res = XML_Parse (pd.parser, buf, buflen, 1);

	if (xml_res == 0) {
                polkit_error_set_error (error, POLKIT_ERROR_POLICY_FILE_INVALID,
                                        "%s:%d: parse error: %s",
                                        path, 
                                        (int) XML_GetCurrentLineNumber (pd.parser),
                                        XML_ErrorString (XML_GetErrorCode (pd.parser)));

		XML_ParserFree (pd.parser);
		kit_free (buf);
		goto error;
	}
	XML_ParserFree (pd.parser);
	kit_free (buf);

        polkit_debug ("Loaded configuration file %s", path);

        if (pk_config->top_config_node != NULL)
                config_node_dump (pk_config->top_config_node);

        return pk_config;

error:
        if (pk_config != NULL)
                polkit_config_unref (pk_config);
        return NULL;
}

/**
 * polkit_config_ref:
 * @pk_config: the object
 * 
 * Increase reference count.
 * 
 * Returns: the object
 **/
PolKitConfig *
polkit_config_ref (PolKitConfig *pk_config)
{
        kit_return_val_if_fail (pk_config != NULL, pk_config);
        pk_config->refcount++;
        return pk_config;
}

/**
 * polkit_config_unref:
 * @pk_config: the object
 * 
 * Decreases the reference count of the object. If it becomes zero,
 * the object is freed. Before freeing, reference counts on embedded
 * objects are decresed by one.
 **/
void
polkit_config_unref (PolKitConfig *pk_config)
{
        kit_return_if_fail (pk_config != NULL);
        pk_config->refcount--;
        if (pk_config->refcount > 0) 
                return;

        if (pk_config->top_config_node != NULL)
                config_node_unref (pk_config->top_config_node);

        kit_free (pk_config);
}

static polkit_bool_t
config_node_match (ConfigNode *node, 
                  PolKitAction *action, 
                  PolKitCaller *caller, 
                  PolKitSession *session)
{
        char *str;
        char *str1;
        char *str2;
        uid_t uid;
        polkit_bool_t match;

        match = FALSE;
        str1 = NULL;
        str2 = NULL;
        switch (node->data.node_match.match_type) {

        case MATCH_TYPE_ACTION:
                if (!polkit_action_get_action_id (action, &str))
                        goto out;
                str1 = kit_strdup (str);
                break;

        case MATCH_TYPE_USER:
                if (caller != NULL) {
                        if (!polkit_caller_get_uid (caller, &uid))
                                goto out;
                } else if (session != NULL) {
                        if (!polkit_session_get_uid (session, &uid))
                                goto out;
                } else
                        goto out;
                
                str1 = kit_strdup_printf ("%d", uid);
                {
                        struct passwd pd;
                        struct passwd* pwdptr=&pd;
                        struct passwd* tempPwdPtr;
                        char pwdbuffer[256];
                        int  pwdlinelen = sizeof(pwdbuffer);
                        
                        if ((getpwuid_r (uid, pwdptr, pwdbuffer, pwdlinelen, &tempPwdPtr)) !=0 )
                                goto out;
                        str2 = kit_strdup (pd.pw_name);
                }
                break;
        }
        
        if (str1 != NULL) {
                if (regexec (&(node->data.node_match.preq), str1, 0, NULL, 0) == 0)
                        match = TRUE;
        }
        if (!match && str2 != NULL) {
                if (regexec (&(node->data.node_match.preq), str2, 0, NULL, 0) == 0)
                        match = TRUE;
        }

out:
        kit_free (str1);
        kit_free (str2);
        return match;
}


/* exactly one of the parameters caller and session must be NULL */
static PolKitResult
config_node_test (ConfigNode *node, 
                  PolKitAction *action, 
                  PolKitCaller *caller, 
                  PolKitSession *session)
{
        polkit_bool_t recurse;
        PolKitResult result;

        recurse = FALSE;
        result = POLKIT_RESULT_UNKNOWN;

        switch (node->node_type) {
        case NODE_TYPE_NOP:
                recurse = FALSE;
                break;
        case NODE_TYPE_TOP:
                recurse = TRUE;
                break;
        case NODE_TYPE_MATCH:
                if (config_node_match (node, action, caller, session))
                        recurse = TRUE;
                break;
        case NODE_TYPE_RETURN:
                result = node->data.node_return.result;
                break;
        default:
                break;
        }

        if (recurse) {
                KitList *i;
                for (i = node->children; i != NULL; i = i->next) {
                        ConfigNode *child_node = i->data;
                        result = config_node_test (child_node, action, caller, session);
                        if (result != POLKIT_RESULT_UNKNOWN) {
                                goto out;
                        }
                }
        }

out:
        return result;
}

/**
 * polkit_config_can_session_do_action:
 * @pk_config: the PolicyKit context
 * @action: the type of access to check for
 * @session: the session in question
 *
 * Determine if the /etc/PolicyKit/PolicyKit.conf configuration file
 * says that a given session can do a given action. 
 *
 * Returns: A #PolKitResult - returns #POLKIT_RESULT_UNKNOWN if there
 * was no match in the configuration file.
 */
PolKitResult
polkit_config_can_session_do_action (PolKitConfig   *pk_config,
                                     PolKitAction   *action,
                                     PolKitSession  *session)
{
        PolKitResult result;
        if (pk_config->top_config_node != NULL)
                result = config_node_test (pk_config->top_config_node, action, NULL, session);
        else
                result = POLKIT_RESULT_UNKNOWN;
        return result;
}

/**
 * polkit_config_can_caller_do_action:
 * @pk_config: the PolicyKit context
 * @action: the type of access to check for
 * @caller: the caller in question
 *
 * Determine if the /etc/PolicyKit/PolicyKit.conf configuration file
 * says that a given caller can do a given action.
 *
 * Returns: A #PolKitResult - returns #POLKIT_RESULT_UNKNOWN if there
 * was no match in the configuration file.
 */
PolKitResult
polkit_config_can_caller_do_action (PolKitConfig   *pk_config,
                                    PolKitAction   *action,
                                    PolKitCaller   *caller)
{
        PolKitResult result;
        if (pk_config->top_config_node != NULL)
                result = config_node_test (pk_config->top_config_node, action, caller, NULL);
        else
                result = POLKIT_RESULT_UNKNOWN;
        return result;
}


static polkit_bool_t
config_node_determine_admin_auth (ConfigNode *node, 
                                  PolKitAction                *action,
                                  PolKitCaller                *caller,
                                  PolKitConfigAdminAuthType   *out_admin_auth_type,
                                  const char                 **out_data)
{
        polkit_bool_t recurse;
        polkit_bool_t result_set;

        recurse = FALSE;
        result_set = FALSE;

        switch (node->node_type) {
        case NODE_TYPE_NOP:
                recurse = FALSE;
                break;
        case NODE_TYPE_TOP:
                recurse = TRUE;
                break;
        case NODE_TYPE_MATCH:
                if (config_node_match (node, action, caller, NULL))
                        recurse = TRUE;
                break;
        case NODE_TYPE_DEFINE_ADMIN_AUTH:
                if (out_admin_auth_type != NULL)
                        *out_admin_auth_type = node->data.node_define_admin_auth.admin_type;
                if (out_data != NULL)
                        *out_data = node->data.node_define_admin_auth.data;
                result_set = TRUE;
                break;
        default:
                break;
        }

        if (recurse) {
                KitList *i;
                for (i = node->children; i != NULL; i = i->next) {
                        ConfigNode *child_node = i->data;

                        result_set = config_node_determine_admin_auth (child_node, 
                                                                       action, 
                                                                       caller, 
                                                                       out_admin_auth_type,
                                                                       out_data) || result_set;
                }
        }

        return result_set;
}

/**
 * polkit_config_determine_admin_auth_type:
 * @pk_config: the PolicyKit context
 * @action: the type of access to check for
 * @caller: the caller in question
 * @out_admin_auth_type: return location for the authentication type
 * @out_data: return location for the match value of the given
 * authentication type. Caller shall not manipulate or free this
 * string.
 *
 * Determine what "Authenticate as admin" means for a given caller and
 * a given action. This basically returns the result of the
 * "define_admin_auth" in the configuration file when drilling down
 * for a specific caller / action.
 *
 * Returns: TRUE if value was returned
 */
polkit_bool_t
polkit_config_determine_admin_auth_type (PolKitConfig                *pk_config,
                                         PolKitAction                *action,
                                         PolKitCaller                *caller,
                                         PolKitConfigAdminAuthType   *out_admin_auth_type,
                                         const char                 **out_data)
{
        if (pk_config->top_config_node != NULL) {
                return config_node_determine_admin_auth (pk_config->top_config_node,
                                                         action, 
                                                         caller, 
                                                         out_admin_auth_type,
                                                         out_data);
        } else {
                return FALSE;
        }
}

#ifdef POLKIT_BUILD_TESTS

static polkit_bool_t
_run_test (void)
{
        return TRUE;
}

KitTest _test_config = {
        "polkit_config",
        NULL,
        NULL,
        _run_test
};

#endif /* POLKIT_BUILD_TESTS */
