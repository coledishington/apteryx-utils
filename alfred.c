/**
 * @file alfred.c
 *
 * Copyright 2015, Allied Telesis Labs New Zealand, Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>
 */
#include <assert.h>
#include <dirent.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlschemas.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <pthread.h>
#include <time.h>
#include <glib.h>
#include <glib-unix.h>
#include <lua.h>
#include <apteryx.h>
#include "common.h"

/* Change the following to alfred*/
#define APTERYX_ALFRED_PID "/var/run/apteryx-alfred.pid"
#define APTERYX_CONFIG_DIR "/etc/apteryx/schema/"
#define SECONDS_TO_MILLI 1000

/* Debug */
bool apteryx_debug = false;

/* An Alfred instance. */
struct alfred_instance_t
{
    /* Lua state */
    lua_State *ls;
    /* List of watches based on path */
    GList *watches;
    /* List of refreshers based on path */
    GList *refreshers;
    /* List of provides based on path */
    GList *provides;
    /* List of indexes based on path */
    GList *indexes;
} alfred_instance_t;
typedef struct alfred_instance_t *alfred_instance;

/* The one and only instance */
alfred_instance alfred_inst = NULL;
static int alfred_apteryx_fd = -1;

int luaopen_apteryx (lua_State *L);

static void
alfred_error (lua_State *ls, int res)
{
    switch (res)
    {
    case LUA_ERRRUN:
        CRITICAL ("LUA: %s\n", lua_tostring (ls, -1));
        break;
    case LUA_ERRSYNTAX:
        CRITICAL ("LUA: %s\n", lua_tostring (ls, -1));
        break;
    case LUA_ERRMEM:
        CRITICAL ("LUA: Memory allocation error\n");
        break;
    case LUA_ERRERR:
        CRITICAL ("LUA: Error handler error\n");
        break;
    case LUA_ERRFILE:
        CRITICAL ("LUA: Couldn't open file\n");
        break;
    default:
        CRITICAL ("LUA: Unknown error\n");
        break;
    }
}

static bool
alfred_call (lua_State *ls, int nresults)
{
    int res, s_0 = lua_gettop (ls);
    size_t fargs = lua_rawlen(ls, -1) - 1;

    /* Load stack with function and its arguments from table */
    for (size_t i = 1; i <= fargs + 1; i++)
    {
        lua_rawgeti(ls, s_0, i);
    }

    res = lua_pcall (ls, (int)(fargs), nresults, 0);
    if (res != 0)
        alfred_error (ls, res);

    if (lua_gettop (ls) != (s_0 + nresults))
    {
        lua_Debug ar;

        /* Push function back on the stack to get some details */
        lua_rawgeti(ls, s_0, 1);
        lua_getinfo(ls, ">nS", &ar);
        printf ("Lua: Stack not zero(%d) after function: %s:%d:%s\n",
                lua_gettop (ls),
                ar.source && ar.source[0] != '\0' ? ar.source : "(unknown)",
                ar.linedefined,
                ar.name && ar.name[0] != '\0' ? ar.name : "(unknown)");
        lua_pop(ls, 1);
    }

    return (res == 0);
}

static bool
alfred_exec (lua_State *ls, const char *script, int nresults)
{
    int res = 0;
    int s_0 = lua_gettop (ls);

    res = luaL_loadstring (ls, script);
    if (res == 0)
        res = lua_pcall (ls, 0, nresults, 0);
    if (res != 0)
        alfred_error (ls, res);

    if (lua_gettop (ls) != (s_0 + nresults))
    {
        ERROR ("Lua: Stack not zero(%d) after script: %s\n",
                lua_gettop (ls), script);
    }

    return (res == 0);
}

static bool
watch_node_changed (const char *path, const char *value)
{
    GList *matches = NULL;
    GList *node = NULL;
    GList *script = NULL;
    GList *scripts = NULL;
    bool ret = false;
    cb_info_t *cb = NULL;

    assert (path);
    assert (alfred_inst);

    matches = cb_match (&alfred_inst->watches, path, CB_MATCH_EXACT |
                        CB_PATH_MATCH_PART | CB_MATCH_WILD_PATH);
    if (matches == NULL)
    {
        ERROR ("ALFRED: No Alfred watch for %s\n", path);
        return false;
    }

    for (node = g_list_first (matches); node != NULL; node = g_list_next (node))
    {
        cb = node->data;
        scripts = (GList *) (long) cb->cb;
        for (script = g_list_first (scripts); script != NULL; script = g_list_next (script))
        {
            lua_pushstring (alfred_inst->ls, path);
            lua_setglobal (alfred_inst->ls, "_path");
            lua_pushstring (alfred_inst->ls, value);
            lua_setglobal (alfred_inst->ls, "_value");
            ret = alfred_exec (alfred_inst->ls, script->data, 0);
        }
    }
    g_list_free_full (matches, (GDestroyNotify) cb_release);
    DEBUG("LUA: Stack:%d Memory:%dkb\n", lua_gettop (alfred_inst->ls),
            lua_gc (alfred_inst->ls, LUA_GCCOUNT, 0));
    DEBUG ("ALFRED WATCH: %s = %s\n", path, value);
    return ret;
}

uint64_t
refresh_node_changed (const char *path)
{
    uint64_t timeout;
    GList *matches = NULL;
    char *script = NULL;
    cb_info_t *cb = NULL;
    int s_0;

    matches = cb_match (&alfred_inst->refreshers, path, CB_MATCH_EXACT | CB_MATCH_WILD_PATH);
    if (matches == NULL)
    {
        ERROR ("ALFRED: No Alfred refresh for %s\n", path);
        return 0;
    }

    cb = g_list_first (matches)->data;
    script = (char *) (long) cb->cb;
    lua_pushstring (alfred_inst->ls, path);
    lua_setglobal (alfred_inst->ls, "_path");
    s_0 = lua_gettop (alfred_inst->ls);
    if (!alfred_exec (alfred_inst->ls, script, 1))
    {
        ERROR ("Lua: Failed to execute refresh script for path: %s\n", path);
    }
    g_list_free_full (matches, (GDestroyNotify) cb_release);
    /* The return value of luaL_dostring is the top value of the stack */
    timeout = lua_tonumber (alfred_inst->ls, -1);
    lua_pop (alfred_inst->ls, 1);

    DEBUG("LUA: Stack:%d Memory:%dkb\n", lua_gettop (alfred_inst->ls),
            lua_gc (alfred_inst->ls, LUA_GCCOUNT, 0));
    if (lua_gettop (alfred_inst->ls) != s_0)
    {
        ERROR ("Lua: Stack not zero(%d) after provide: %s\n",
                lua_gettop (alfred_inst->ls), path);
    }
    return timeout;
}

char *
provide_node_changed (const char *path)
{
    const char *const_value = NULL;
    char *ret = NULL;
    GList *matches = NULL;
    char *script = NULL;
    cb_info_t *cb = NULL;
    int s_0;

    matches = cb_match (&alfred_inst->provides, path, CB_MATCH_EXACT | CB_MATCH_WILD_PATH);
    if (matches == NULL)
    {
        ERROR ("ALFRED: No Alfred provide for %s\n", path);
        return NULL;
    }

    cb = g_list_first (matches)->data;
    script = (char *) (long) cb->cb;
    lua_pushstring (alfred_inst->ls, path);
    lua_setglobal (alfred_inst->ls, "_path");
    s_0 = lua_gettop (alfred_inst->ls);
    if (!alfred_exec (alfred_inst->ls, script, 1))
    {
        ERROR ("Lua: Failed to execute provide script for path: %s\n", path);
    }
    g_list_free_full (matches, (GDestroyNotify) cb_release);
    /* The return value of luaL_dostring is the top value of the stack */
    const_value = lua_tostring (alfred_inst->ls, -1);
    lua_pop (alfred_inst->ls, 1);
    ret = g_strdup (const_value);
    DEBUG("LUA: Stack:%d Memory:%dkb\n", lua_gettop (alfred_inst->ls),
            lua_gc (alfred_inst->ls, LUA_GCCOUNT, 0));
    if (lua_gettop (alfred_inst->ls) != s_0)
    {
        ERROR ("Lua: Stack not zero(%d) after provide: %s\n",
                lua_gettop (alfred_inst->ls), path);
    }
    return ret;
}

static GList *
index_node_changed (const char *path)
{
    char *script = NULL;
    const char *tmp_path = NULL;
    char *tmp_path2 = NULL;
    GList *ret = NULL;
    GList *matches = NULL;
    cb_info_t *cb = NULL;
    int s_0;

    matches = cb_match (&alfred_inst->indexes, path, CB_MATCH_EXACT | CB_MATCH_WILD_PATH);
    if (matches == NULL)
    {
        ERROR ("ALFRED: No Alfred index for %s\n", path);
        return NULL;
    }
    cb = g_list_first (matches)->data;
    script = (char *) (long) cb->cb;
    lua_pushstring (alfred_inst->ls, path);
    lua_setglobal (alfred_inst->ls, "_path");
    s_0 = lua_gettop (alfred_inst->ls);
    if (!alfred_exec (alfred_inst->ls, script, 1))
    {
        ERROR ("Lua: Failed to execute index script for path: %s\n", path);
    }
    g_list_free_full (matches, (GDestroyNotify) cb_release);

    if (lua_gettop (alfred_inst->ls))
    {
        if (lua_istable(alfred_inst->ls, -1))
        {
            lua_pushnil (alfred_inst->ls);
            while (lua_next(alfred_inst->ls, -2) != 0)
            {
                tmp_path = lua_tostring (alfred_inst->ls, -1);
                tmp_path2 = strdup (tmp_path);
                ret = g_list_append (ret, tmp_path2);
                /* Removes 'value'; keeps 'key' for next iteration */
                lua_pop (alfred_inst->ls, 1);
            }
            lua_pop (alfred_inst->ls, 1);
        }
    }
    DEBUG("LUA: Stack:%d Memory:%dkb\n", lua_gettop(alfred_inst->ls),
            lua_gc (alfred_inst->ls, LUA_GCCOUNT, 0));
    if (lua_gettop (alfred_inst->ls) != s_0)
    {
        ERROR ("Lua: Stack not zero(%d) after index: %s\n",
                lua_gettop (alfred_inst->ls), path);
    }
    return ret;
}

static void
alfred_register_watches (gpointer value, gpointer user_data)
{
    cb_info_t *cb = (cb_info_t *) value;
    int install = GPOINTER_TO_INT (user_data);

    if ((install && !apteryx_watch (cb->path, watch_node_changed)) ||
        (!install && !apteryx_unwatch (cb->path, watch_node_changed)))
    {
        ERROR ("Failed to (un)register watch for path %s\n", cb->path);
    }
}

static void
alfred_register_refresh (gpointer value, gpointer user_data)
{
    cb_info_t *cb = (cb_info_t *) value;
    int install = GPOINTER_TO_INT (user_data);

    if ((install && !apteryx_refresh (cb->path, refresh_node_changed)) ||
        (!install && !apteryx_unrefresh (cb->path, refresh_node_changed)))
    {
        ERROR ("Failed to (un)register refresh for path %s\n", cb->path);
    }
}

static void
alfred_register_provide (gpointer value, gpointer user_data)
{
    cb_info_t *cb = (cb_info_t *) value;
    int install = GPOINTER_TO_INT (user_data);

    if ((install && !apteryx_provide (cb->path, provide_node_changed)) ||
        (!install && !apteryx_unprovide (cb->path, provide_node_changed)))
    {
        ERROR ("Failed to (un)register provide for path %s\n", cb->path);
    }
}

static void
alfred_register_index (gpointer value, gpointer user_data)
{
    cb_info_t *cb = (cb_info_t *) value;
    int install = GPOINTER_TO_INT (user_data);

    if ((install && !apteryx_index (cb->path, index_node_changed)) ||
        (!install && !apteryx_unindex (cb->path, index_node_changed)))
    {
        ERROR ("Failed to (un)register provide for path %s\n", cb->path);
    }
}

static bool
destroy_watches (gpointer value, gpointer rpc)
{
    cb_info_t *cb = (cb_info_t *) value;
    GList *scripts = (GList *) (long) cb->cb;
    DEBUG ("XML: Destroy watches for path %s\n", cb->path);

    g_list_free_full (scripts, g_free);
    cb_destroy (cb);
    cb_release (cb);
    return true;
}

static bool
destroy_refresher (gpointer value, gpointer rpc)
{
    cb_info_t *cb = (cb_info_t *) value;
    char *script = (char *) (long) cb->cb;
    DEBUG ("XML: Destroy refresher for path %s\n", cb->path);

    g_free (script);
    cb_destroy (cb);
    cb_release (cb);
    return true;
}

static bool
destroy_provides (gpointer value, gpointer rpc)
{
    cb_info_t *cb = (cb_info_t *) value;
    char *script = (char *) (long) cb->cb;
    DEBUG ("XML: Destroy provides for path %s\n", cb->path);

    g_free (script);
    cb_destroy (cb);
    cb_release (cb);
    return true;
}

static bool
destroy_indexes (gpointer value, gpointer rpc)
{
    cb_info_t *cb = (cb_info_t *) value;
    char *script = (char *) (long) cb->cb;
    DEBUG ("XML: Destroy indexes for path %s\n", cb->path);

    g_free (script);
    cb_destroy (cb);
    cb_release (cb);
    return true;
}

static bool
node_is_leaf (xmlNode *node)
{
    for (xmlNode *n = node->children; n; n = n->next)
    {
        if (n->type == XML_ELEMENT_NODE && strcmp ((const char *) n->name, "NODE") == 0)
            return false;
    }
    return true;
}

static bool
process_node (alfred_instance alfred, xmlNode *node, char *parent)
{
    xmlChar *name = NULL;
    xmlChar *content = NULL;
    char *path = NULL;
    char *tmp_content = NULL;
    GList *matches = NULL;
    GList *scripts = NULL;
    cb_info_t *cb;
    bool res = true;

    assert (alfred);

    /* Ignore fluff */
    if (!node || node->type != XML_ELEMENT_NODE)
        return true;

    /* Process this node */
    if (strcmp ((const char *) node->name, "NODE") == 0)
    {
        /* Find node name and path */
        name = xmlGetProp (node, (xmlChar *) "name");
        if (parent)
            path = g_strdup_printf ("%s/%s", parent, name);
        else
            path = g_strdup_printf ("/%s", name);

        DEBUG ("XML: %s: %s (%s)\n", node->name, name, path);
    }
    else if (strcmp ((const char *) node->name, "WATCH") == 0)
    {
        content = xmlNodeGetContent (node);
        tmp_content = g_strdup ((char *) content);
        /* If the node is a leaf or ends in a '*' don't add another '*' */
        if (node_is_leaf (node->parent) || parent[strlen (parent) - 1] == '*')
        {
            path = g_strdup (parent);
        }
        else
        {
            path = g_strdup_printf ("%s/*", parent);
        }

        if (alfred->watches)
        {
            matches = cb_match (&alfred->watches, path, CB_MATCH_EXACT);
        }
        if (matches == NULL)
        {
            scripts = g_list_append (scripts, tmp_content);
            cb = cb_create (&alfred->watches, "", (const char *) path, 0,
                            (uint64_t) (long) scripts);
        }
        else
        {
            /* A watch already exists on that exact path */
            cb = matches->data;
            scripts = (GList *) (long) cb->cb;
            scripts = g_list_append (scripts, tmp_content);
            g_list_free_full (matches, (GDestroyNotify) cb_release);
        }
        DEBUG ("XML: %s: (%s)\n", node->name, cb->path);
    }
    else if (strcmp ((const char *) node->name, "SCRIPT") == 0)
    {
        bool ret = false;
        content = xmlNodeGetContent (node);
        DEBUG ("XML: %s: %s\n", node->name, content);
        ret = alfred_exec (alfred->ls, (char *) content, 0);
        if (!ret)
        {
            res = false;
            goto exit;
        }
    }
    else if (strcmp ((const char *) node->name, "REFRESH") == 0)
    {
        content = xmlNodeGetContent (node);
        tmp_content = g_strdup ((char *) content);
        DEBUG ("REFRESH: %s, XML STR: %s\n", parent, content);

        /* If the node is a leaf or ends in a '*' don't add another '*' */
        if (node_is_leaf (node->parent) || parent[strlen (parent) - 1] == '*')
        {
            path = g_strdup (parent);
        }
        else
        {
            path = g_strdup_printf ("%s/*", parent);
        }
        if (path)
        {
            cb = cb_create (&alfred->refreshers, "", (const char *) path, 0,
                            (uint64_t) (long) tmp_content);
        }
    }
    else if (strcmp ((const char *) node->name, "PROVIDE") == 0)
    {
        content = xmlNodeGetContent (node);
        tmp_content = g_strdup ((char *) content);
        DEBUG ("PROVIDE: %s, XML STR: %s\n", parent, content);

        /* If the node is a leaf or ends in a '*' don't add another '*' */
        if (node_is_leaf (node->parent) || parent[strlen (parent) - 1] == '*')
        {
            path = g_strdup (parent);
        }
        else
        {
            path = g_strdup_printf ("%s/*", parent);
        }
        if (path)
        {
            cb = cb_create (&alfred->provides, "", (const char *) path, 0,
                            (uint64_t) (long) tmp_content);
        }
    }
    else if (strcmp ((const char *) node->name, "INDEX") == 0)
    {
        content = xmlNodeGetContent (node);
        tmp_content = g_strdup ((char *) content);
        DEBUG ("INDEX: XML STR: %s\n", content);

        /* If the node is a leaf or ends in a '*' don't add another '*' */
        if (node_is_leaf (node->parent) || parent[strlen (parent) - 1] == '*')
        {
            path = g_strdup (parent);
        }
        else
        {
            path = g_strdup_printf ("%s/*", parent);
        }
        if (path)
        {
            cb = cb_create (&alfred->indexes, "", (const char *) path, 0,
                            (uint64_t) (long) tmp_content);
        }
    }
    /* Process children */
    for (xmlNode *n = node->children; n; n = n->next)
    {
        if (!process_node (alfred, n, path))
        {
            res = false;
            goto exit;
        }
    }

  exit:
    if (path)
        g_free (path);
    if (name)
        xmlFree (name);
    if (content)
        xmlFree (content);
    return res;
}

static bool
load_config_files (alfred_instance alfred, const char *path)
{
    struct dirent *entry;
    DIR *dir;
    bool res = true;

    /* Find all the XML files in this folder */
    dir = opendir (path);
    if (dir == NULL)
    {
        DEBUG ("XML: Failed to open \"%s\"", path);
        return false;
    }

    /* Load all libraries first */
    for (entry = readdir (dir); entry; entry = readdir (dir))
    {
        const char *ext = strrchr (entry->d_name, '.');
        if (ext && strcmp (".lua", ext) == 0)
        {
            char *filename = g_strdup_printf ("%s/%s", path, entry->d_name);
            int error;

            DEBUG ("ALFRED: Load Lua file \"%s\"\n", filename);

            /* Execute the script */
            lua_getglobal (alfred->ls, "debug");
            lua_getfield (alfred->ls, -1, "traceback");
            error = luaL_loadfile (alfred->ls, filename);
            if (error == 0)
                error = lua_pcall (alfred->ls, 0, 0, 0);
            if (error != 0)
                alfred_error (alfred->ls, error);
            g_free (filename);

            while (lua_gettop (alfred->ls))
                lua_pop (alfred->ls, 1);

            /* Stop processing files if there has been an error */
            if (error != 0)
            {
                res = false;
                goto exit;
            }
        }
    }
    rewinddir (dir);

    /* Load all XML files */
    for (entry = readdir (dir); entry; entry = readdir (dir))
    {
        const char *ext = strchr (entry->d_name, '.');
        if (ext && ((strcmp (".xml", ext) == 0) || (strcmp (".xml.gz", ext) == 0)))
        {
            /* Full path */
            char *filename = g_strdup_printf ("%s%s%s", path,
                path[strlen (path) - 1] == '/' ? "" : "/", entry->d_name);

            DEBUG ("ALFRED: Parse XML file \"%s\"\n", filename);
            /* Parse the file */
            xmlDoc *doc = xmlParseFile (filename);
            if (doc == NULL)
            {
                ERROR ("ALFRED: Invalid file \"%s\"\n", filename);
                g_free (filename);
                res = false;
                goto exit;
            }
            res = process_node (alfred, xmlDocGetRootElement (doc), NULL);
            xmlFreeDoc (doc);
            g_free (filename);

            /* Stop processing files if there has been an error */
            if (!res)
                goto exit;
        }
    }

  exit:
    closedir (dir);
    return res;
}

GList *delayed_work = NULL;
struct delayed_work_s {
    guint id;
    int call;
    char *script;
};

static void
dw_destroy (gpointer arg1)
{
    struct delayed_work_s *dw = (struct delayed_work_s *) arg1;
    luaL_unref (alfred_inst->ls, LUA_REGISTRYINDEX, dw->call);
    dw->call = LUA_NOREF;
    g_free (dw->script);
    g_free (dw);
}

static gboolean
delayed_work_process (gpointer arg1)
{
    struct delayed_work_s *dw = (struct delayed_work_s *) arg1;

    /* Remove the script to be run */
    delayed_work = g_list_remove (delayed_work, dw);

    if (dw->script)
    {
        /* Execute the script */
        alfred_exec (alfred_inst->ls, dw->script, 0);
    }
    else
    {
        lua_rawgeti (alfred_inst->ls, LUA_REGISTRYINDEX, dw->call);
        alfred_call (alfred_inst->ls, 0);
        lua_pop (alfred_inst->ls, 0);
    }

    return false;
}

static void
delayed_work_add (lua_State *ls, bool reset_timer)
{
    int call_args = 0;
    bool found = false;
    const char *script = NULL;
    struct delayed_work_s *dw = NULL;

    if (lua_isstring (ls, 2))
    {
        script = lua_tostring(ls, 2);
    }
    else /* lua_isfunction(ls, 2) */
    {
        call_args = lua_gettop (ls) - 1;
    }

    for (GList * iter = delayed_work; iter && !found; iter = g_list_next (iter))
    {
        dw = (struct delayed_work_s *) iter->data;
        if (script)
        {
            found = dw->script && strcmp (script, dw->script) == 0;
        }
        else if (lua_isfunction (ls, 2)
                 && dw->call != LUA_NOREF && dw->call != LUA_REFNIL)
        {
            size_t len;
            bool call_same = true;

            lua_rawgeti(ls, LUA_REGISTRYINDEX, dw->call);
            /* Try to avoid comparing calls */
            len = lua_rawlen(ls, -1);
            if (((size_t)call_args) != len)
            {
                lua_pop(ls, 1);
                continue;
            }
            for (size_t i = 0; call_same && i < call_args; i++)
            {
                lua_rawgeti(ls, -1, i + 1);
                /* lua_compare has the usual meaning in lua, tables with
                 * the same keys and values will not be counted as the same,
                 * unless metamethods are changed.
                 */
                call_same = lua_compare(ls, i + 2, -1, LUA_OPEQ);
                lua_pop(ls, 1);
            }
            found = call_same;
            lua_pop(ls, 1);
        }
        if (found && reset_timer)
        {
            delayed_work = g_list_remove (delayed_work, dw);
            g_source_remove (dw->id);
        }
    }

    if (!found || reset_timer)
    {
        struct delayed_work_s *dw = (struct delayed_work_s *) g_malloc0 (sizeof (struct delayed_work_s));

        if (script)
        {
            dw->script = g_strdup (script);
        }
        else
        {
            /* Transfer stack (past delay) into the argument table */
            lua_newtable(ls);
            lua_pushvalue(ls, 2);
            lua_rawseti(ls, -2, 1);
            lua_replace(ls, 2);
            for (int i = lua_gettop (ls); i > 2; i--)
            {
                lua_rawseti(ls, 2, i - 1);
            }
            dw->call = luaL_ref(ls, LUA_REGISTRYINDEX);
        }
        delayed_work = g_list_append (delayed_work, dw);
        dw->id = g_timeout_add_full (G_PRIORITY_DEFAULT,
                                     lua_tonumber (ls, 1) * SECONDS_TO_MILLI,
                                     delayed_work_process, (gpointer) dw, dw_destroy);
    }
}

static int
validate_script_or_function_args (lua_State *ls, const char *funct)
{
    bool success = true;

    if (!lua_isnumber (ls, 1))
    {
        ERROR ("First argument to %s must be a number\n", funct);
        success = false;
    }
    if (lua_isstring (ls, 2))
    {
        if (lua_gettop (ls) != 2)
        {
            ERROR ("%s takes 2 arguements\n", funct);
            success = false;
        }
    }
    else if (!lua_isfunction (ls, 2))
    {
        ERROR ("Second argument to %s must be a string or Lua function\n", funct);
        success = false;
    }
    return success;
}

static int
rate_limit (lua_State *ls)
{
    if (validate_script_or_function_args (ls, "Alfred.rate_limit()"))
    {
        delayed_work_add (ls, false);
    }
    return 0;
}

static int
after_quiet (lua_State *ls)
{
    if (validate_script_or_function_args (ls, "Alfred.after_quiet()"))
    {
        delayed_work_add (ls, true);
    }
    return 0;
}

static void
alfred_shutdown (void)
{
    assert (alfred_inst);

    if (alfred_inst->watches)
    {
        g_list_foreach (alfred_inst->watches, (GFunc) alfred_register_watches,
                        GINT_TO_POINTER (0));
        g_list_foreach (alfred_inst->watches, (GFunc) destroy_watches, NULL);
        g_list_free (alfred_inst->watches);
    }

    if (alfred_inst->refreshers)
    {
        g_list_foreach (alfred_inst->refreshers, (GFunc) alfred_register_refresh,
                        GINT_TO_POINTER (0));
        g_list_foreach (alfred_inst->refreshers, (GFunc) destroy_refresher, NULL);
        g_list_free (alfred_inst->refreshers);
    }

    if (alfred_inst->provides)
    {
        g_list_foreach (alfred_inst->provides, (GFunc) alfred_register_provide,
                        GINT_TO_POINTER (0));
        g_list_foreach (alfred_inst->provides, (GFunc) destroy_provides, NULL);
        g_list_free (alfred_inst->provides);
    }

    if (alfred_inst->indexes)
    {
        g_list_foreach (alfred_inst->indexes, (GFunc) alfred_register_index,
                        GINT_TO_POINTER (0));
        g_list_foreach (alfred_inst->indexes, (GFunc) destroy_indexes, NULL);
        g_list_free (alfred_inst->indexes);
    }

    if (alfred_inst->ls)
        lua_close (alfred_inst->ls);

    g_free (alfred_inst);
    alfred_inst = NULL;
    return;
}

void
alfred_init (const char *path)
{
    assert (path);

    /* Malloc memory for the new service */
    alfred_inst = (alfred_instance) g_malloc0 (sizeof (*alfred_inst));
    if (!alfred_inst)
    {
        CRITICAL ("ALFRED: No memory for alfred instance\n");
        goto error;
    }

    /* Initialise the Lua state */
    alfred_inst->ls = luaL_newstate ();
    if (!alfred_inst->ls)
    {
        CRITICAL ("XML: Failed to instantiate Lua interpreter\n");
        goto error;
    }

    /* Load required libraries */
    luaL_openlibs (alfred_inst->ls);
    if (luaopen_apteryx (alfred_inst->ls))
    {
        /* Provide global access to the Apteryx library */
        lua_setglobal (alfred_inst->ls, "apteryx");
    }

    /* Load the apteryx-xml API if available
       api = require("apteryx.xml").api("/etc/apteryx/schema/")
     */
    if (luaL_dostring (alfred_inst->ls, "require('api')") != 0)
    {
        ERROR ("Lua: Failed to require('api')\n");
    }

    /* Add the rate_limit,after_quiet functions to a Lua table so it can be called using Lua */
    lua_newtable (alfred_inst->ls);
    lua_pushcfunction (alfred_inst->ls, rate_limit);
    lua_setfield (alfred_inst->ls, -2, "rate_limit");
    lua_pushcfunction (alfred_inst->ls, after_quiet);
    lua_setfield (alfred_inst->ls, -2, "after_quiet");
    lua_setglobal (alfred_inst->ls, "Alfred");

    /* Parse files in the config path */
    if (!load_config_files (alfred_inst, path))
    {
        goto error;
    }

    /* Register watches */
    g_list_foreach (alfred_inst->watches, (GFunc) alfred_register_watches, GINT_TO_POINTER (1));

    /* Register refreshers */
    g_list_foreach (alfred_inst->refreshers, (GFunc) alfred_register_refresh, GINT_TO_POINTER (1));

    /* Register provides */
    g_list_foreach (alfred_inst->provides, (GFunc) alfred_register_provide, GINT_TO_POINTER (1));

    /* Register indexes */
    g_list_foreach (alfred_inst->indexes, (GFunc) alfred_register_index, GINT_TO_POINTER (1));

    return;
error:
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    return;
}

void
test_simple_watch ()
{
    FILE *library = NULL;
    FILE *data = NULL;
    char *test_str = NULL;

    /* Create library file + XML */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                "function test_library_function(test_str)\n"
                "  test_value = test_str\n"
                "end\n"
                );
        fclose (library);
    }

    data = fopen ("alfred_test.xml", "w");
    g_assert (data != NULL);
    if (data)
    {
        fprintf (data, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<MODULE xmlns=\"https://github.com/alliedtelesis/apteryx\"\n"
                   "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                   "  xsi:schemaLocation=\"https://github.com/alliedtelesis/apteryx\n"
                   "  https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd\">\n"
                   "  <SCRIPT>\n"
                   "  function test_node_change(new_value)\n"
                   "    test_library_function(new_value)\n"
                   "  end\n"
                   "  </SCRIPT>\n"
                   "  <NODE name=\"test\">\n"
                   "    <NODE name=\"set_node\" mode=\"rw\"  help=\"Set this node to test the watch function\">\n"
                   "      <WATCH>test_node_change(_value)</WATCH>\n"
                   "    </NODE>\n"
                   "  </NODE>\n"
                   "</MODULE>\n");
        fclose (data);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Trigger Action */
        apteryx_set ("/test/set_node", "Goodnight moon");
        sleep (1);

        /* Check output */
        lua_getglobal (alfred_inst->ls, "test_value");
        if (!lua_isnil (alfred_inst->ls, -1))
        {
            test_str = strdup (lua_tostring (alfred_inst->ls, -1));
        }
        lua_pop (alfred_inst->ls, 1);

        g_assert (test_str && strcmp (test_str, "Goodnight moon") == 0);
        apteryx_set ("/test/set_node", NULL);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
    unlink ("alfred_test.xml");
    free (test_str);
}

void
test_native_watch ()
{
    FILE *library = NULL;
    char *test_str = NULL;

    /* Create library file only */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                "function test_node_change(path,value)\n"
                "  test_value = value\n"
                "  apteryx.unwatch('/test/set_node', test_node_change)\n"
                "end\n"
                "apteryx.watch('/test/set_node', test_node_change)\n"
                );
        fclose (library);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Trigger Action */
        apteryx_set ("/test/set_node", "Goodnight moon");
        sleep (1);

        /* Check output */
        lua_getglobal (alfred_inst->ls, "test_value");
        if (!lua_isnil (alfred_inst->ls, -1))
        {
            test_str = strdup (lua_tostring (alfred_inst->ls, -1));
        }
        lua_pop (alfred_inst->ls, 1);
        g_assert (test_str && strcmp (test_str, "Goodnight moon") == 0);
        apteryx_set ("/test/set_node", NULL);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
    free (test_str);
}

void
test_dir_watch ()
{
    FILE *library = NULL;
    FILE *data = NULL;
    char *test_str = NULL;
    char *test_path = NULL;

    /* Create library file + XML */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                "function test_library_function(p, v)\n"
                "  test_value = v\n"
                "  test_path = p\n"
                "end\n"
                );
        fclose (library);
    }

    data = fopen ("alfred_test.xml", "w");
    g_assert (data != NULL);
    if (data)
    {
        fprintf (data, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<MODULE xmlns=\"https://github.com/alliedtelesis/apteryx\"\n"
                   "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                   "  xsi:schemaLocation=\"https://github.com/alliedtelesis/apteryx\n"
                   "  https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd\">\n"
                   "  <SCRIPT>\n"
                   "  function test_dir_change(path, new_value)\n"
                   "    test_library_function(path, new_value)\n"
                   "  end\n"
                   "  </SCRIPT>\n"
                   "  <NODE name=\"test\">\n"
                   "    <WATCH>test_dir_change(_path, _value)</WATCH>\n"
                   "    <NODE name=\"set_node\" mode=\"rw\"  help=\"Set this node to test the watch function\"/>\n"
                   "    <NODE name=\"deeper\">\n"
                   "      <NODE name=\"set_node\" mode=\"rw\"  help=\"Set this node to test the deeper function\"/>\n"
                   "    </NODE>\n"
                   "  </NODE>\n"
                   "</MODULE>\n");
        fclose (data);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Trigger Action */
        apteryx_set ("/test/set_node", "Goodnight cow jumping over the moon");
        sleep (1);

        /* Check output */
        lua_getglobal (alfred_inst->ls, "test_value");
        if (!lua_isnil (alfred_inst->ls, -1))
            test_str = strdup (lua_tostring (alfred_inst->ls, -1));
        lua_pop (alfred_inst->ls, 1);
        lua_getglobal (alfred_inst->ls, "test_path");
        if (!lua_isnil (alfred_inst->ls, -1))
            test_path = strdup (lua_tostring (alfred_inst->ls, -1));
        lua_pop (alfred_inst->ls, 1);

        g_assert (test_path && strcmp (test_path, "/test/set_node") == 0);
        g_assert (test_str && strcmp (test_str, "Goodnight cow jumping over the moon") == 0);
        free (test_path);
        free (test_str);

        /* Trigger Action */
        apteryx_set ("/test/deeper/set_node", "Goodnight bears");
        sleep (1);

        /* Check output */
        lua_getglobal (alfred_inst->ls, "test_value");
        if (!lua_isnil (alfred_inst->ls, -1))
            test_str = strdup (lua_tostring (alfred_inst->ls, -1));
        lua_pop (alfred_inst->ls, 1);

        lua_getglobal (alfred_inst->ls, "test_path");
        if (!lua_isnil (alfred_inst->ls, -1))
            test_path = strdup (lua_tostring (alfred_inst->ls, -1));
        lua_pop (alfred_inst->ls, 1);

        g_assert (test_path && strcmp (test_path, "/test/deeper/set_node") == 0);
        g_assert (test_str && strcmp (test_str, "Goodnight bears") == 0);

        apteryx_set ("/test/set_node", NULL);
        apteryx_set ("/test/deeper/set_node", NULL);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
    unlink ("alfred_test.xml");
    free (test_str);
    free (test_path);
}

void
test_simple_provide ()
{
    FILE *library = NULL;
    FILE *data = NULL;
    char *test_str = NULL;

    /* Create library file + XML */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                "function test_library_function(path)\n"
                "  return \"hello \"..path\n"
                "end\n"
                );
        fclose (library);
    }

    data = fopen ("alfred_test.xml", "w");
    g_assert (data != NULL);
    if (data)
    {
        fprintf (data, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<MODULE xmlns=\"https://github.com/alliedtelesis/apteryx\"\n"
                   "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                   "  xsi:schemaLocation=\"https://github.com/alliedtelesis/apteryx\n"
                   "  https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd\">\n"
                   "  <SCRIPT>\n"
                   "  function test_provide(path)\n"
                   "    return test_library_function(path)\n"
                   "  end\n"
                   "  </SCRIPT>\n"
                   "  <NODE name=\"test\">\n"
                   "    <NODE name=\"set_node\" mode=\"rw\"  help=\"Get this node to test the provide function\">\n"
                   "      <PROVIDE>return test_provide(_path)</PROVIDE>\n"
                   "    </NODE>\n"
                   "  </NODE>\n"
                   "</MODULE>\n");
        fclose (data);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        sleep (1);

        /* Trigger provide */
        test_str = apteryx_get ("/test/set_node");
        g_assert (test_str && strcmp (test_str, "hello /test/set_node") == 0);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
    unlink ("alfred_test.xml");
    free (test_str);
}

void
test_native_provide ()
{
    FILE *library = NULL;
    char *test_str = NULL;

    /* Create library file only */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                "function test_node_provide(path)\n"
                "  apteryx.unprovide('/test/set_node', test_node_provide)\n"
                "  return \"hello \"..path\n"
                "end\n"
                "apteryx.provide('/test/set_node', test_node_provide)\n"
                );
        fclose (library);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Trigger provide */
        test_str = apteryx_get ("/test/set_node");
        g_assert (test_str && strcmp (test_str, "hello /test/set_node") == 0);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
    free (test_str);
}

void
test_simple_refresh ()
{
    FILE *data = NULL;
    char *test_str = NULL;
    GList *paths = NULL;

    /* Create XML */
    data = fopen ("alfred_test.xml", "w");
    g_assert (data != NULL);
    if (data)
    {
        fprintf (data, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<MODULE xmlns=\"https://github.com/alliedtelesis/apteryx\"\n"
                   "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                   "  xsi:schemaLocation=\"https://github.com/alliedtelesis/apteryx\n"
                   "  https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd\">\n"
                   "  <SCRIPT>\n"
                   "  count = 0\n"
                   "  function test_refresh(path)\n"
                   "    apteryx.set('/test/eth0/refresh/count', tostring(count))\n"
                   "    count = count + 1\n"
                   "    return 500000\n"
                   "  end\n"
                   "  </SCRIPT>\n"
                   "  <NODE name=\"test\">\n"
                   "    <NODE name=\"*\">\n"
                   "      <NODE name=\"refresh\">\n"
                   "        <NODE name=\"count\" mode=\"rw\" help=\"Get this node to test the refresh function\" />\n"
                   "        <REFRESH>return test_refresh(_path)</REFRESH>\n"
                   "      </NODE>\n"
                   "    </NODE>\n"
                   "  </NODE>\n"
                   "</MODULE>\n");
        fclose (data);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        sleep (1);

        /* Search */
        paths = apteryx_search ("/test/eth0/refresh/");
        g_assert (g_list_length (paths) == 1);
        g_assert (paths && (strcmp ((char *) paths->data, "/test/eth0/refresh/count") == 0));
        g_list_free_full (paths, free);
        usleep (500000);

        /* Trigger provide */
        test_str = apteryx_get ("/test/eth0/refresh/count");
        g_assert (test_str && strcmp (test_str, "1") == 0);
        free (test_str);
        test_str = apteryx_get ("/test/eth0/refresh/count");
        g_assert (test_str && strcmp (test_str, "1") == 0);
        free (test_str);
        usleep (500000);
        test_str = apteryx_get ("/test/eth0/refresh/count");
        g_assert (test_str && strcmp (test_str, "2") == 0);
        free (test_str);
        apteryx_set ("/test/eth0/refresh/count", NULL);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.xml");
}

void
test_native_refresh ()
{
    FILE *library = NULL;
    char *test_str = NULL;
    GList *paths = NULL;

    /* Create library file only */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                "count = 0\n"
                "function test_refresh(path)\n"
                "  if count == 2 then\n"
                "    apteryx.unrefresh('/test/refresh/*', test_refresh)\n"
                "  end\n"
                "  apteryx.set('/test/refresh/count', tostring(count))\n"
                "  count = count + 1\n"
                "  return 500000\n"
                "end\n"
                "apteryx.refresh('/test/refresh/*', test_refresh)\n"
                );
        fclose (library);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Search */
        paths = apteryx_search ("/test/refresh/");
        g_assert (g_list_length (paths) == 1);
        g_assert (paths && (strcmp ((char *) paths->data, "/test/refresh/count") == 0));
        g_list_free_full (paths, free);
        usleep (500000);

        /* Get */
        test_str = apteryx_get ("/test/refresh/count");
        g_assert (test_str && strcmp (test_str, "1") == 0);
        free (test_str);
        test_str = apteryx_get ("/test/refresh/count");
        g_assert (test_str && strcmp (test_str, "1") == 0);
        free (test_str);
        usleep (500000);
        test_str = apteryx_get ("/test/refresh/count");
        g_assert (test_str && strcmp (test_str, "2") == 0);
        free (test_str);
        apteryx_set ("/test/refresh/count", NULL);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
}

void
test_simple_index ()
{
    FILE *library = NULL;
    FILE *data = NULL;
    GList *paths = NULL;

    /* Create library file + XML */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
            "function test_library_function()\n"
            "  return {\"Goodnight light\", \"and the red balloon\"}\n"
            "end\n"
            );
        fclose (library);
    }

    data = fopen ("alfred_test.xml", "w");
    g_assert (data != NULL);
    if (data)
    {
        fprintf (data, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<MODULE xmlns=\"https://github.com/alliedtelesis/apteryx\"\n"
                   "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                   "  xsi:schemaLocation=\"https://github.com/alliedtelesis/apteryx\n"
                   "  https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd\">\n"
                   "  <SCRIPT>\n"
                   "  function test_index(path)\n"
                   "    return test_library_function()\n"
                   "  end\n"
                   "  </SCRIPT>\n"
                   "  <NODE name=\"test\">\n"
                   "    <NODE name=\"*\" help=\"Set this node to test the watch function\">\n"
                   "      <INDEX>return test_index(_path)</INDEX>\n"
                   "      <NODE name=\"id\" mode=\"rw\"/>\n"
                   "    </NODE>\n"
                   "  </NODE>\n"
                   "</MODULE>\n");
        fclose (data);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Trigger Action */
        paths = apteryx_search ("/test/");

        g_assert (g_list_length (paths) == 2);
        g_assert (paths && (strcmp ((char *) paths->data, "Goodnight light") == 0 ||
                strcmp ((char *) paths->data, "and the red balloon") == 0));
        g_assert (paths && paths->next &&
                (strcmp ((char *) paths->next->data, "and the red balloon") == 0 ||
                strcmp ((char *) paths->next->data, "Goodnight light") == 0));
        g_assert (paths && paths->next &&
                (strcmp ((char *) paths->data, (char *) paths->next->data) != 0));
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
        alfred_inst = NULL;
    }
    unlink ("alfred_test.lua");
    unlink ("alfred_test.xml");
    if (paths)
    {
        g_list_free_full (paths, free);
    }
}

void
test_native_index ()
{
    FILE *library = NULL;
    GList *paths = NULL;

    /* Create library file only */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                "function test_node_index(path)\n"
                "  apteryx.unindex('/test', test_node_index)\n"
                "  return {\"Goodnight light\", \"and the red balloon\"}\n"
                "end\n"
                "apteryx.index('/test', test_node_index)\n"
                );
        fclose (library);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Trigger Action */
        paths = apteryx_search ("/test/");

        g_assert (g_list_length (paths) == 2);
        g_assert (paths && (strcmp ((char *) paths->data, "Goodnight light") == 0 ||
                strcmp ((char *) paths->data, "and the red balloon") == 0));
        g_assert (paths && paths->next &&
                (strcmp ((char *) paths->next->data, "and the red balloon") == 0 ||
                strcmp ((char *) paths->next->data, "Goodnight light") == 0));
        g_assert (paths && paths->next &&
                (strcmp ((char *) paths->data, (char *) paths->next->data) != 0));
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
        alfred_inst = NULL;
    }
    unlink ("alfred_test.lua");
    if (paths)
    {
        g_list_free_full (paths, free);
    }
}

/* Glib unit test */
void
test_rate_limit ()
{
    FILE *library = NULL;
    FILE *data = NULL;
    char *test_str = NULL;
    lua_Integer test_count;

    apteryx_init (false);
    /* Create library file + XML */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
            "count = 0\n\n"
            "function test_library_function(test_str)\n"
            "  test_value = test_str\n"
            "  count = count + 1\n"
            "end\n"
            );
        fclose (library);
    }

    data = fopen ("alfred_test.xml", "w");
    g_assert (data != NULL);
    if (data)
    {
        fprintf (data, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<MODULE xmlns=\"https://github.com/alliedtelesis/apteryx\"\n"
                   "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                   "  xsi:schemaLocation=\"https://github.com/alliedtelesis/apteryx\n"
                   "  https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd\">\n"
                   "  <SCRIPT>\n"
                   "  function test_node_change(new_value)\n"
                   "    test_library_function(new_value)\n"
                   "  end\n"
                   "  </SCRIPT>\n"
                   "  <NODE name=\"test\">\n"
                   "    <NODE name=\"set_node\" mode=\"rw\"  help=\"Set this node to test the watch function\">\n"
                   "      <WATCH>Alfred.rate_limit(0.1,'test_node_change(_value)')</WATCH>\n"
                   "    </NODE>\n"
                   "  </NODE>\n"
                   "</MODULE>\n");
        fclose (data);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        /* Trigger Action */
        int count = 0;

        while (count < 50)
        {
            apteryx_set ("/test/set_node", "Goodnight scoot");
            count++;
        }

        sleep (1);
        /* Check output */
        lua_getglobal (alfred_inst->ls, "test_value");
        if (!lua_isnil (alfred_inst->ls, -1))
        {
            test_str = strdup (lua_tostring (alfred_inst->ls, -1));
        }
        lua_pop (alfred_inst->ls, 1);

        lua_getglobal (alfred_inst->ls, "count");
        test_count = lua_tointeger(alfred_inst->ls, -1);
        lua_pop (alfred_inst->ls, 1);

        g_assert (test_str && strcmp (test_str, "Goodnight scoot") == 0);
        g_assert (test_count < 50);
        apteryx_set ("/test/set_node", NULL);
        sleep(1);
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
    unlink ("alfred_test.xml");
    free (test_str);
}

/* Glib unit test */
void
test_after_quiet ()
{
    FILE *library = NULL;
    FILE *data = NULL;

    apteryx_init (false);
    /* Create library file + XML */
    library = fopen ("alfred_test.lua", "w");
    g_assert (library != NULL);
    if (library)
    {
        fprintf (library,
                 "count = 0\n\n"
                 "function test_library_function(test_str)\n"
                 "  test_value = test_str\n"
                 "  count = count + 1\n"
                 "end\n\n"
                 "function test_library_function2(...)\n"
                 "  local args = table.pack(...)\n"
                 "  test_value = \"CONCATED:\"\n"
                 "  for i=1, args.n do\n"
                 "    test_value = test_value .. tostring(args[i])\n"
                 "  end\n"
                 "end\n");
        fclose (library);
    }

    data = fopen ("alfred_test.xml", "w");
    g_assert (data != NULL);
    if (data)
    {
        fprintf (data, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<MODULE xmlns=\"https://github.com/alliedtelesis/apteryx\"\n"
                    "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                    "  xsi:schemaLocation=\"https://github.com/alliedtelesis/apteryx\n"
                    "  https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd\">\n"
                    "  <SCRIPT>\n"
                    "  function test_node_change(new_value)\n"
                    "    test_library_function(new_value)\n"
                    "  end\n"
                    "  </SCRIPT>\n"
                    "  <NODE name=\"test\">\n"
                    "    <NODE name=\"set_script_node\" mode=\"rw\">\n"
                    "      <WATCH>Alfred.after_quiet(0.1, 'test_node_change(_value)')</WATCH>\n"
                    "    </NODE>\n"
                    "    <NODE name=\"set_function_node\" mode=\"rw\">\n"
                    "      <WATCH>Alfred.after_quiet(0.1, test_library_function2)</WATCH>\n"
                    "    </NODE>\n"
                    "    <NODE name=\"set_function_arg_node\" mode=\"rw\">\n"
                    "      <WATCH>Alfred.after_quiet(0.1, test_library_function, _value)</WATCH>\n"
                    "    </NODE>\n"
                    "    <NODE name=\"set_function_many_args_node\" mode=\"rw\">\n"
                    "      <WATCH>Alfred.after_quiet(0.1, test_library_function2, nil, 1, '\\\\2', 3, false, _value, true, nil, 4, '5', 6)</WATCH>\n"
                    "    </NODE>\n"
                    "  </NODE>\n"
                    "</MODULE>\n");
        fclose (data);
    }

    /* Init */
    alfred_init ("./");
    g_assert (alfred_inst != NULL);
    if (alfred_inst)
    {
        char *test_str = NULL;
        lua_Integer test_count;
        struct {
            const char *node;
            const char *check;
        } tests[4];

        tests[0].node = "/test/set_script_node";
        tests[0].check = "Goodnight scoot";
        tests[1].node = "/test/set_function_node";
        tests[1].check = "CONCATED:";
        tests[2].node = "/test/set_function_arg_node";
        tests[2].check = "Goodnight scoot";
        tests[3].node = "/test/set_function_many_args_node";
        tests[3].check = "CONCATED:nil1\\23falseGoodnight scoottruenil456";

        for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        {
            int count = 0;

            while (count < 50)
            {
                apteryx_set (tests[i].node, "Goodnight scoot");
                count++;
            }

            sleep (1);
            /* Check output */
            lua_getglobal (alfred_inst->ls, "test_value");
            if (!lua_isnil (alfred_inst->ls, -1))
            {
                test_str = strdup (lua_tostring (alfred_inst->ls, -1));
            }
            lua_pop (alfred_inst->ls, 1);

            lua_getglobal (alfred_inst->ls, "count");
            test_count = lua_tointeger(alfred_inst->ls, -1);
            lua_pop (alfred_inst->ls, 1);

            g_assert (test_str && strcmp (test_str, tests[i].check) == 0);
            g_assert (test_count == 1);

            /* Reset Lua variables */
            lua_pushnil(alfred_inst->ls);
            lua_setglobal(alfred_inst->ls, "test_value");
            lua_pushinteger (alfred_inst->ls, (lua_Integer)0);
            lua_setglobal(alfred_inst->ls, "count");

            apteryx_set (tests[i].node, NULL);
            free (test_str);
            sleep(1);
        }
    }

    /* Clean up */
    if (alfred_inst)
    {
        alfred_shutdown ();
    }
    unlink ("alfred_test.lua");
    unlink ("alfred_test.xml");
}

static gboolean
process_apteryx (GIOChannel *source, GIOCondition condition, gpointer data)
{
    assert (alfred_inst);
    luaL_loadstring (alfred_inst->ls, "apteryx.process()");
    int res = lua_pcall (alfred_inst->ls, 0, 0, 0);
    if (res != 0)
        alfred_error (alfred_inst->ls, res);
    uint8_t dummy = 0;
    if (read (alfred_apteryx_fd, &dummy, 1) == 0)
    {
        ERROR ("Poll/Read error: %s\n", strerror (errno));
    }
    return true;
}

static gboolean
termination_handler (gpointer arg1)
{
    GMainLoop *loop = (GMainLoop *) arg1;
    g_main_loop_quit (loop);
    return false;
}

void
help (char *app_name)
{
    printf ("Usage: %s [-h] [-b] [-d] [-p <pidfile>] [-c <configdir>] [-u <filter>]\n"
            "  -h   show this help\n"
            "  -b   background mode\n"
            "  -d   enable verbose debug\n"
            "  -m   memory profiling\n"
            "  -p   use <pidfile> (defaults to "APTERYX_ALFRED_PID")\n"
            "  -c   use <configdir> (defaults to "APTERYX_CONFIG_DIR")\n"
            ,app_name);
}

int
main (int argc, char *argv[])
{
    const char *pid_file = APTERYX_ALFRED_PID;
    const char *config_dir = APTERYX_CONFIG_DIR;
    int i = 0;
    bool background = false;
    FILE *fp = NULL;
    GMainLoop *loop = NULL;
    bool unit_test = false;

    /* Parse options */
    while ((i = getopt (argc, argv, "hdbp:c:mu::")) != -1)
    {
        switch (i)
        {
        case 'd':
            apteryx_debug = true;
            background = false;
            break;
        case 'b':
            background = true;
            break;
        case 'p':
            pid_file = optarg;
            break;
        case 'c':
            config_dir = optarg;
            break;
        case 'u':
            unit_test = true;
            break;
        case '?':
        case 'h':
        default:
            help (argv[0]);
            return 0;
        }
    }

    /* Daemonize */
    if (!unit_test && background && fork () != 0)
    {
        /* Parent */
        return 0;
    }

    /* Initialise Apteryx client library in single threaded mode */
    apteryx_init (apteryx_debug);
    alfred_apteryx_fd = apteryx_process (true);
    g_io_add_watch (g_io_channel_unix_new (alfred_apteryx_fd),
                    G_IO_IN, process_apteryx, NULL);

    cb_init ();

    if (unit_test)
    {
        pthread_t main_thread;
        pthread_attr_t attr;

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        g_test_init (&argc, &argv, NULL);
        g_test_add_func ("/test_simple_watch", test_simple_watch);
        g_test_add_func ("/test_native_watch", test_native_watch);
        g_test_add_func ("/test_dir_watch", test_dir_watch);
        g_test_add_func ("/test_simple_refresh", test_simple_refresh);
        g_test_add_func ("/test_native_refresh", test_native_refresh);
        g_test_add_func ("/test_simple_provide", test_simple_provide);
        g_test_add_func ("/test_native_provide", test_native_provide);
        g_test_add_func ("/test_simple_index", test_simple_index);
        g_test_add_func ("/test_native_index", test_native_index);
        g_test_add_func ("/test_rate_limit", test_rate_limit);
        g_test_add_func ("/test_after_quiet", test_after_quiet);

        loop = g_main_loop_new (NULL, true);
        g_unix_signal_add (SIGINT, termination_handler, loop);
        g_unix_signal_add (SIGTERM, termination_handler, loop);
        pthread_create (&main_thread, &attr, (void *) g_main_loop_run, loop);
        pthread_join(main_thread, NULL);
        g_test_run();
        pthread_cancel(main_thread);
        pthread_attr_destroy(&attr);
        goto exit;
    }
    else
    {
        /* Create the alfred glists */
        alfred_init (config_dir);
        if (!alfred_inst)
            goto exit;
    }

    /* Create pid file */
    if (background)
    {
        fp = fopen (pid_file, "w");
        if (!fp)
        {
            ERROR ("Failed to create PID file %s\n", pid_file);
            goto exit;
        }
        fprintf (fp, "%d\n", getpid ());
        fclose (fp);
    }

    loop = g_main_loop_new (NULL, true);

    /* Handle SIGTERM/SIGINT/SIGPIPE gracefully */
    g_unix_signal_add (SIGINT, termination_handler, loop);
    g_unix_signal_add (SIGTERM, termination_handler, loop);
    signal (SIGPIPE, SIG_IGN);

    /* Loop while not terminated */
    g_main_loop_run (loop);

  exit:
    /* Free the glib main loop */
    if (loop)
    {
        g_main_loop_unref (loop);
    }

    /* Clean alfreds */
    if (alfred_inst)
        alfred_shutdown ();

    /* Cleanup client library */
    apteryx_shutdown ();

    /* Remove the pid file */
    if (background)
        unlink (pid_file);

    return 0;
}
