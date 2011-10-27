/*
    Copyright (C) 2011  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "internal_libreport.h"

static GHashTable *user_settings;
static char *conf_path;

static bool create_parentdir(char *path)
{
    bool ret;
    char *c;

    /* in-place dirname() */
    for (c = path + strlen(path); c > path && *c != '/'; c--)
        ;
    if (*c != '/')
        return false;
    *c = '\0';

    ret = make_dir_recursive(path, 0755);
    *c = '/'; /* restore path back */

    return ret;
}

/* Returns false if write failed */
bool save_conf_file(const char *path, map_string_h *settings)
{
    bool ret;
    FILE *out;
    char *temp_path, *name, *value;
    GHashTableIter iter;

    ret = false, out = NULL;

    temp_path = xasprintf("%s.tmp", path);

    if (!create_parentdir(temp_path) || !(out = fopen(temp_path, "w")))
        goto cleanup;

    g_hash_table_iter_init(&iter, settings);
    while (g_hash_table_iter_next(&iter, (void**)&name, (void**)&value))
        fprintf(out, "%s = \"%s\"\n", name, value);

    fclose(out);
    out = NULL;

    if (!rename(temp_path, path))
        goto cleanup;

    ret = true; /* success */

cleanup:
    if (out)
        fclose(out);
    free(temp_path);

    return ret;
}

static char *get_conf_path(const char *name)
{
    char *HOME = getenv("HOME"), *s, *conf;

    s = xasprintf("%s/%s.conf", ".abrt/settings", name);
    conf = concat_path_file(HOME, s);
    free(s);
    return conf;
}

bool save_user_settings()
{
    if (!conf_path || !user_settings)
        return true;

    return save_conf_file(conf_path, user_settings);
}

bool load_user_settings(const char *application_name)
{
    if (conf_path)
        free(conf_path);
    conf_path = get_conf_path(application_name);

    if (user_settings)
        g_hash_table_destroy(user_settings);
    user_settings = g_hash_table_new_full(
            /*hash_func*/ g_str_hash,
            /*key_equal_func:*/ g_str_equal,
            /*key_destroy_func:*/ free,
            /*value_destroy_func:*/ free);

    return load_conf_file(conf_path, user_settings, false);
}

void set_user_setting(const char *name, const char *value)
{
    if (value)
        g_hash_table_replace(user_settings, xstrdup(name), xstrdup(value));
    else
        g_hash_table_remove(user_settings, name);
}

const char *get_user_setting(const char *name)
{
    return g_hash_table_lookup(user_settings, name);
}
