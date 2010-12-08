/*
    Copyright (C) 2009, 2010  Red Hat, Inc.

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
#include <termios.h>
#include <glib.h>
#include "report.h"
#include "run-command.h"
#include "dbus.h"
#include "abrtlib.h"

/* Field separator for the crash report file that is edited by user. */
#define FIELD_SEP "%----"

/*
 * Trims whitespace characters both from left and right side of a string.
 * Modifies the string in-place. Returns the trimmed string.
 */
static char *trim(char *str)
{
    if (!str)
        return NULL;

    // Remove leading spaces.
    char *ibuf;
    ibuf = skip_whitespace(str);
    int i = strlen(ibuf);
    if (str != ibuf)
        memmove(str, ibuf, i + 1);

    // Remove trailing spaces.
    while (--i >= 0)
    {
        if (!isspace(str[i]))
            break;
    }
    str[++i] = '\0';
    return str;
}

/*
 * Escapes the field content string to avoid confusion with file comments.
 * Returned field must be free()d by caller.
 */
static char *escape(const char *str)
{
    // Determine the size of resultant string.
    // Count the required number of escape characters.
    // 1. NEWLINE followed by #
    // 2. NEWLINE followed by \# (escaped version)
    const char *ptr = str;
    bool newline = true;
    int count = 0;
    while (*ptr)
    {
        if (newline)
        {
            if (*ptr == '#')
                ++count;
            if (*ptr == '\\' && ptr[1] == '#')
                ++count;
        }

        newline = (*ptr == '\n');
        ++ptr;
    }

    // Copy the input string to the resultant string, and escape all
    // occurences of \# and #.
    char *result = (char*)xmalloc(strlen(str) + 1 + count);

    const char *src = str;
    char *dest = result;
    newline = true;
    while (*src)
    {
        if (newline)
        {
            if (*src == '#')
                *dest++ = '\\';
            else if (*src == '\\' && *(src + 1) == '#')
                *dest++ = '\\';
        }

        newline = (*src == '\n');
        *dest++ = *src++;
    }
    *dest = '\0';
    return result;
}

/*
 * Removes all comment lines, and unescapes the string previously escaped
 * by escape(). Works in-place.
 */
static void remove_comments_and_unescape(char *str)
{
    char *src = str, *dest = str;
    bool newline = true;
    while (*src)
    {
        if (newline)
        {
            if (*src == '#')
            { // Skip the comment line!
                while (*src && *src != '\n')
                    ++src;

                if (*src == '\0')
                    break;

                ++src;
                continue;
            }
            if (*src == '\\'
                    && (src[1] == '#' || (src[1] == '\\' && src[2] == '#'))
               ) {
                ++src; // Unescape escaped char.
            }
        }

        newline = (*src == '\n');
        *dest++ = *src++;
    }
    *dest = '\0';
}

/*
 * Writes a field of crash report to a file.
 * Field must be writable.
 */
static void write_crash_report_field(FILE *fp, crash_data_t *crash_data,
        const char *field, const char *description)
{
    const struct crash_item *value = get_crash_data_item_or_NULL(crash_data, field);
    if (!value)
    {
        // exit silently, all fields are optional for now
        //error_msg("Field %s not found", field);
        return;
    }

    if (value->flags & CD_FLAG_SYS)
    {
        error_msg("Cannot update field %s because it is a system value", field);
        return;
    }

    fprintf(fp, "%s%s\n", FIELD_SEP, field);

    fprintf(fp, "%s\n", description);
    if (!(value->flags & CD_FLAG_ISEDITABLE))
        fprintf(fp, _("# This field is read only\n"));

    char *escaped_content = escape(value->content);
    fprintf(fp, "%s\n", escaped_content);
    free(escaped_content);
}

/*
 * Saves the crash report to a file.
 * Parameter 'fp' must be opened before write_crash_report is called.
 * Returned value:
 *  If the report is successfully stored to the file, a zero value is returned.
 *  On failure, nonzero value is returned.
 */
static void write_crash_report(crash_data_t *report, FILE *fp)
{
    fprintf(fp, "# Please check this report. Lines starting with '#' will be ignored.\n"
            "# Lines starting with '%%----' separate fields, please do not delete them.\n\n");

    write_crash_report_field(fp, report, FILENAME_COMMENT,
            _("# Describe the circumstances of this crash below"));
    write_crash_report_field(fp, report, FILENAME_REPRODUCE,
            _("# How to reproduce the crash?"));
    write_crash_report_field(fp, report, FILENAME_BACKTRACE,
            _("# Backtrace\n# Check that it does not contain any sensitive data (passwords, etc.)"));
    write_crash_report_field(fp, report, FILENAME_DUPHASH, "# DUPHASH");
    write_crash_report_field(fp, report, FILENAME_ARCHITECTURE, _("# Architecture"));
    write_crash_report_field(fp, report, FILENAME_CMDLINE, _("# Command line"));
    write_crash_report_field(fp, report, FILENAME_COMPONENT, _("# Component"));
    write_crash_report_field(fp, report, FILENAME_COREDUMP, _("# Core dump"));
    write_crash_report_field(fp, report, FILENAME_EXECUTABLE, _("# Executable"));
    write_crash_report_field(fp, report, FILENAME_KERNEL, _("# Kernel version"));
    write_crash_report_field(fp, report, FILENAME_PACKAGE, _("# Package"));
    write_crash_report_field(fp, report, FILENAME_REASON, _("# Reason of crash"));
    write_crash_report_field(fp, report, FILENAME_RELEASE, _("# Release string of the operating system"));
}

/*
 * Updates appropriate field in the report from the text. The text can
 * contain multiple fields.
 * Returns:
 *  0 if no change to the field was detected.
 *  1 if the field was changed.
 *  Changes to read-only fields are ignored.
 */
static int read_crash_report_field(const char *text, crash_data_t *report,
        const char *field)
{
    char separator[sizeof("\n" FIELD_SEP)-1 + strlen(field) + 2]; // 2 = '\n\0'
    sprintf(separator, "\n%s%s\n", FIELD_SEP, field);
    const char *textfield = strstr(text, separator);
    if (!textfield)
        return 0; // exit silently because all fields are optional

    textfield += strlen(separator);
    int length = 0;
    const char *end = strstr(textfield, "\n" FIELD_SEP);
    if (!end)
        length = strlen(textfield);
    else
        length = end - textfield;

    struct crash_item *value = get_crash_data_item_or_NULL(report, field);
    if (!value)
    {
        error_msg("Field %s not found", field);
        return 0;
    }

    if (value->flags & CD_FLAG_SYS)
    {
        error_msg("Cannot update field %s because it is a system value", field);
        return 0;
    }

    // Do not change noneditable fields.
    if (!(value->flags & CD_FLAG_ISEDITABLE))
        return 0;

    // Compare the old field contents with the new field contents.
    char newvalue[length + 1];
    strncpy(newvalue, textfield, length);
    newvalue[length] = '\0';
    trim(newvalue);

    char oldvalue[strlen(value->content) + 1];
    strcpy(oldvalue, value->content);
    trim(oldvalue);

    // Return if no change in the contents detected.
    if (strcmp(newvalue, oldvalue) == 0)
        return 0;

    free(value->content);
    value->content = xstrdup(newvalue);
    return 1;
}

/*
 * Updates the crash report 'report' from the text. The text must not contain
 * any comments.
 * Returns:
 *  0 if no field was changed.
 *  1 if any field was changed.
 *  Changes to read-only fields are ignored.
 */
static int read_crash_report(crash_data_t *report, const char *text)
{
    int result = 0;
    result |= read_crash_report_field(text, report, FILENAME_COMMENT);
    result |= read_crash_report_field(text, report, FILENAME_REPRODUCE);
    result |= read_crash_report_field(text, report, FILENAME_BACKTRACE);
    result |= read_crash_report_field(text, report, FILENAME_DUPHASH);
    result |= read_crash_report_field(text, report, FILENAME_ARCHITECTURE);
    result |= read_crash_report_field(text, report, FILENAME_CMDLINE);
    result |= read_crash_report_field(text, report, FILENAME_COMPONENT);
    result |= read_crash_report_field(text, report, FILENAME_COREDUMP);
    result |= read_crash_report_field(text, report, FILENAME_EXECUTABLE);
    result |= read_crash_report_field(text, report, FILENAME_KERNEL);
    result |= read_crash_report_field(text, report, FILENAME_PACKAGE);
    result |= read_crash_report_field(text, report, FILENAME_REASON);
    result |= read_crash_report_field(text, report, FILENAME_RELEASE);
    return result;
}

/**
 * Ensures that the fields needed for editor are present in the crash data.
 * Fields: comments, how to reproduce.
 */
static void create_fields_for_editor(crash_data_t *crash_data)
{
    if (!get_crash_data_item_or_NULL(crash_data, FILENAME_COMMENT))
        add_to_crash_data_ext(crash_data, FILENAME_COMMENT, "", CD_FLAG_TXT + CD_FLAG_ISEDITABLE);

    if (!get_crash_data_item_or_NULL(crash_data, FILENAME_REPRODUCE))
        add_to_crash_data_ext(crash_data, FILENAME_REPRODUCE, "1. \n2. \n3. \n", CD_FLAG_TXT + CD_FLAG_ISEDITABLE);
}

/**
 * Runs external editor.
 * Returns:
 *  0 if the launch was successful
 *  1 if it failed. The error reason is logged using error_msg()
 */
static int launch_editor(const char *path)
{
    const char *editor, *terminal;

    editor = getenv("ABRT_EDITOR");
    if (!editor)
        editor = getenv("VISUAL");
    if (!editor)
        editor = getenv("EDITOR");

    terminal = getenv("TERM");
    if (!editor && (!terminal || strcmp(terminal, "dumb") == 0))
    {
        error_msg(_("Cannot run vi: $TERM, $VISUAL and $EDITOR are not set"));
        return 1;
    }

    if (!editor)
        editor = "vi";

    char *args[3];
    args[0] = (char*)editor;
    args[1] = (char*)path;
    args[2] = NULL;
    run_command(args);

    return 0;
}

/**
 * Returns:
 *  0 on success, crash data has been updated
 *  2 on failure, unable to create, open, or close temporary file
 *  3 on failure, cannot launch text editor
 */
static int run_report_editor(crash_data_t *crash_data)
{
    /* Open a temporary file and write the crash report to it. */
    char filename[] = "/tmp/abrt-report.XXXXXX";
    int fd = mkstemp(filename);
    if (fd == -1) /* errno is set */
    {
        perror_msg("can't generate temporary file name");
        return 2;
    }

    FILE *fp = fdopen(fd, "w");
    if (!fp) /* errno is set */
    {
        perror_msg("can't open '%s' to save the crash report", filename);
        return 2;
    }

    write_crash_report(crash_data, fp);

    if (fclose(fp)) /* errno is set */
    {
        perror_msg("can't close '%s'", filename);
        return 2;
    }

    // Start a text editor on the temporary file.
    if (launch_editor(filename) != 0)
        return 3; /* exit with error */

    // Read the file back and update the report from the file.
    fp = fopen(filename, "r");
    if (!fp) /* errno is set */
    {
        perror_msg("can't open '%s' to read the crash report", filename);
        return 2;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *text = (char*)xmalloc(size + 1);
    if (fread(text, 1, size, fp) != size)
    {
        error_msg("can't read '%s'", filename);
        return 2;
    }
    text[size] = '\0';
    if (fclose(fp) != 0) /* errno is set */
    {
        perror_msg("can't close '%s'", filename);
        return 2;
    }

    // Delete the tempfile.
    if (unlink(filename) == -1) /* errno is set */
    {
        perror_msg("can't unlink %s", filename);
    }

    remove_comments_and_unescape(text);
    // Updates the crash report from the file text.
    int report_changed = read_crash_report(crash_data, text);
    free(text);
    if (report_changed)
        puts(_("\nThe report has been updated"));
    else
        puts(_("\nNo changes were detected in the report"));

    return 0;
}

/**
 * Asks user for a text response.
 * @param question
 *  Question displayed to user.
 * @param result
 *  Output array.
 * @param result_size
 *  Maximum byte count to be written.
 */
static void read_from_stdin(const char *question, char *result, int result_size)
{
    assert(result_size > 1);
    printf("%s", question);
    fflush(NULL);
    if (NULL == fgets(result, result_size, stdin))
        result[0] = '\0';
    // Remove the newline from the login.
    strchrnul(result, '\n')[0] = '\0';
}

/**
 * Asks a [y/n] question on stdin/stdout.
 * Returns true if the answer is yes, false otherwise.
 */
static bool ask_yesno(const char *question)
{
    /* The response might take more than 1 char in non-latin scripts. */
    const char *yes = _("y");
    const char *no = _("N");
    printf("%s [%s/%s]: ", question, yes, no);
    fflush(NULL);

    char answer[16];
    fgets(answer, sizeof(answer), stdin);
    /* Use strncmp here because the answer might contain a newline as
       the last char. */
    return 0 == strncmp(answer, yes, strlen(yes));
}

/* Returns true if echo has been changed from another state. */
static bool set_echo(bool enabled)
{
    if (isatty(STDIN_FILENO) == 0)
    {
        /* Clean errno, which is set by isatty. */
        errno = 0;
        return false;
    }

    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) < 0)
        return false;

    /* No change needed. */
    if ((bool)(t.c_lflag & ECHO) == enabled)
        return false;

    if (enabled)
        t.c_lflag |= ECHO;
    else
        t.c_lflag &= ~ECHO;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0)
        perror_msg_and_die("tcsetattr");

    return true;
}

static void free_map_string_t(gpointer data)
{
    delete (map_string_t *)data;
}

/**
 * Gets reporter plugin settings.
 * @param reporters
 *   List of reporter names. Settings of these reporters are handled.
 * @return settings
 *   A structure filled with reporter plugin settings.
 *   It's GHashTable<char *, map_plugin_t *> and must be passed to
 *   g_hash_table_destroy();
 */
static GHashTable *get_reporter_plugin_settings(const vector_string_t& reporters)
{
    /* First of all, load system-wide report plugin settings. */
    GHashTable *settings = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 free, free_map_string_t);

    for (vector_string_t::const_iterator it = reporters.begin(); it != reporters.end(); ++it)
    {
        map_string_t *single_plugin_settings = new map_string_t;
        *single_plugin_settings = call_GetPluginSettings(it->c_str());

        // Copy the received settings as defaults.
        // Plugins won't work without it, if some value is missing
        // they use their default values for all fields.
        g_hash_table_replace(settings, xstrdup(it->c_str()), single_plugin_settings);
    }

    /* Second, load user-specific settings, which override
       the system-wide settings. */
    struct passwd* pw = getpwuid(geteuid());
    const char* homedir = pw ? pw->pw_dir : NULL;
    if (homedir)
    {
        GHashTableIter iter;
        char *key;
        map_string_t *value;
        g_hash_table_iter_init(&iter, settings);
        while (g_hash_table_iter_next(&iter, (void**)&key, (void**)&value))
        {
            /* Load plugin config in the home dir. Do not skip lines
             * with empty value (but containing a "key="),
             * because user may want to override password
             * from /etc/abrt/plugins/*.conf, but he prefers to
             * enter it every time he reports. */
            map_string_h *single_plugin_settings = new_map_string();
            char *path = xasprintf("%s/.abrt/%s.conf", homedir, key);
            bool success = load_conf_file(path, single_plugin_settings, /*skip key w/o values:*/ false);
            free(path);
            if (!success)
            {
                free_map_string(single_plugin_settings);
                continue;
            }

            /* Merge user's plugin settings into already loaded settings */
            GHashTableIter iter2;
            char *key2;
            char *value2;
            g_hash_table_iter_init(&iter2, single_plugin_settings);
            while (g_hash_table_iter_next(&iter2, (void**)&key2, (void**)&value2))
                (*value)[key2] = xstrdup(value2);

            free_map_string(single_plugin_settings);
        }
    }
    return settings;
}

/**
 *  Asks user for missing login information
 */
static void ask_for_missing_settings(const char *plugin_name, map_string_t &single_plugin_settings)
{
    // Login information is missing.
    bool loginMissing = single_plugin_settings.find("Login") != single_plugin_settings.end()
        && 0 == strcmp(single_plugin_settings["Login"].c_str(), "");
    bool passwordMissing = single_plugin_settings.find("Password") != single_plugin_settings.end()
        && 0 == strcmp(single_plugin_settings["Password"].c_str(), "");
    if (!loginMissing && !passwordMissing)
        return;

    // Read the missing information and push it to plugin settings.
    printf(_("Wrong settings were detected for plugin %s\n"), plugin_name);
    char result[64];
    if (loginMissing)
    {
        read_from_stdin(_("Enter your login: "), result, 64);
        single_plugin_settings["Login"] = std::string(result);
    }
    if (passwordMissing)
    {
        bool changed = set_echo(false);
        read_from_stdin(_("Enter your password: "), result, 64);
        if (changed)
            set_echo(true);

        // Newline was not added by pressing Enter because ECHO was disabled, so add it now.
        puts("");
        single_plugin_settings["Password"] = std::string(result);
    }
}

/* Reports the crash with corresponding crash_id over DBus. */
int report(const char *crash_id, int flags)
{
    int old_logmode = logmode;
    if (flags & CLI_REPORT_SILENT_IF_NOT_FOUND)
        logmode = 0;
    // Ask for an initial report.
    crash_data_t *crash_data = call_CreateReport(crash_id);
    logmode = old_logmode;
    if (!crash_data || g_hash_table_size(crash_data) == 0)
    {
        free_crash_data(crash_data);
        return -1;
    }

    const char *rating_str = get_crash_item_content_or_NULL(crash_data, FILENAME_RATING);
    unsigned rating = rating_str ? xatou(rating_str) : 4;

    /* Open text editor and give a chance to review the backtrace etc. */
    if (!(flags & CLI_REPORT_BATCH))
    {
        create_fields_for_editor(crash_data);
        int result = run_report_editor(crash_data);
        if (result != 0)
        {
            free_crash_data(crash_data);
            return result;
        }
    }

    /* Get possible reporters associated with this particular crash. */
    const char *events = get_crash_item_content_or_NULL(crash_data, CD_EVENTS);
    vector_string_t reporters;
    if (events) while (*events)
    {
        const char *end = strchrnul(events, '\n');
        if (strncmp(events, "report", 6) == 0
         && (events[6] == '\0' || events[6] == '_')
        ) {
            char *tmp = xstrndup(events, end - events);
            reporters.push_back(tmp);
            free(tmp);
        }
        events = end;
        if (!*events)
            break;
        events++;
    }

    /* Get settings */
    GHashTable *reporters_settings = get_reporter_plugin_settings(reporters);

    int errors = 0;
    int plugins = 0;
    if (flags & CLI_REPORT_BATCH)
    {
        puts(_("Reporting..."));
        report_status_t r = call_Report(crash_data, reporters, reporters_settings);
        report_status_t::iterator it = r.begin();
        while (it != r.end())
        {
            vector_string_t &v = it->second;
            printf("%s: %s\n", it->first.c_str(), v[REPORT_STATUS_IDX_MSG].c_str());
            plugins++;
            if (v[REPORT_STATUS_IDX_FLAG] == "0")
                errors++;
            it++;
        }
    }
    else
    {
        /* For every reporter, ask if user really wants to report using it. */
        for (vector_string_t::const_iterator it = reporters.begin(); it != reporters.end(); ++it)
        {
            char question[255];
            snprintf(question, 255, _("Report using %s?"), it->c_str());
            if (!ask_yesno(question))
            {
                puts(_("Skipping..."));
                continue;
            }

            map_string_t *settings = (map_string_t *)g_hash_table_lookup(reporters_settings, it->c_str());
            if (settings)
            {
                map_string_t::iterator rating_setting = settings->find("RatingRequired");
                if (rating_setting != settings->end()
                        && string_to_bool(rating_setting->second.c_str())
                        && rating < 3)
                {
                    puts(_("Reporting disabled because the backtrace is unusable"));

                    const char *package = get_crash_item_content_or_NULL(crash_data, FILENAME_PACKAGE);
                    if (package && package[0])
                        printf(_("Please try to install debuginfo manually using the command: \"debuginfo-install %s\" and try again\n"), package);

                    plugins++;
                    errors++;
                    continue;
                }
            }
            else
            {
                puts(_("Error loading reporter settings"));
                plugins++;
                errors++;
                continue;
            }

            ask_for_missing_settings(it->c_str(), *settings);

            vector_string_t cur_reporter(1, *it);
            report_status_t r = call_Report(crash_data, cur_reporter, reporters_settings);
            assert(r.size() == 1); /* one reporter --> one report status */
            vector_string_t &v = r.begin()->second;
            printf("%s: %s\n", r.begin()->first.c_str(), v[REPORT_STATUS_IDX_MSG].c_str());
            plugins++;
            if (v[REPORT_STATUS_IDX_FLAG] == "0")
                errors++;
        }
    }

    g_hash_table_destroy(reporters_settings);
    free_crash_data(crash_data);
    printf(_("Crash reported via %d report events (%d errors)\n"), plugins, errors);
    return errors != 0;
}
