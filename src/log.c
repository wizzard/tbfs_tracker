/*
 * Copyright (C) 2012-2013 Paul Ionkin <paul.ionkin@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "wutils.h"
#include <stdio.h>
#include <syslog.h>

static gboolean use_syslog = FALSE;

// prints a message string to stdout
// XXX: extend it (syslog, etc)
void logger_log_msg (G_GNUC_UNUSED const gchar *file, G_GNUC_UNUSED gint line, G_GNUC_UNUSED const gchar *func, 
        LogLevel level, const gchar *subsystem,
        const gchar *format, ...)
{
    va_list args;
    char out_str[1024];
    struct tm cur;
    char ts[50];
    time_t t;
    struct tm *cur_p;

    if (log_level < level)
        return;

    t = time (NULL);
    gmtime_r (&t, &cur);
    cur_p = &cur;
    if (!strftime (ts, sizeof (ts), "%H:%M:%S", cur_p)) {
        ts[0] = '\0';
    }

    va_start (args, format);
        g_vsnprintf (out_str, sizeof (out_str), format, args);
    va_end (args);

    if (log_level == LOG_debug) {
        if (level == LOG_err)
            g_fprintf (stdout, "%s \033[1;31m[%s]\033[0m  (%s %s:%d) %s\n", ts, subsystem, func, file, line, out_str);
        else
            g_fprintf (stdout, "%s [%s] (%s %s:%d) %s\n", ts, subsystem, func, file, line, out_str);
    }
    else {
        if (use_syslog)
            syslog (log_level == LOG_msg ? LOG_INFO : LOG_ERR, "%s", out_str);
        else {
            if (level == LOG_err)
                g_fprintf (stdout, "\033[1;31mERROR!\033[0m %s\n", out_str);
            else
                g_fprintf (stdout, "%s\n", out_str);
        }
    }
}

void logger_destroy (void)
{
    if (use_syslog)
        closelog ();
}

void logger_set_syslog (gboolean use)
{
    use_syslog = use;
}
