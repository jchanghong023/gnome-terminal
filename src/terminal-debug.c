/*
 * Copyright (C) 2002,2003 Red Hat, Inc.
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <glib.h>

#include "terminal-debug.h"

TerminalDebugFlags _terminal_debug_flags;

#define TERMINAL_LOG_ERRORS (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL)

static void
terminal_log_handler (const gchar *domain,
                      GLogLevelFlags level,
                      const gchar *message,
                      gpointer user_data)
{
  if (level & TERMINAL_LOG_ERRORS)
    g_log_default_handler (domain, level, message, user_data);
}

#if GLIB_CHECK_VERSION (2, 50, 0)
static GLogWriterOutput
terminal_log_writer (GLogLevelFlags level,
                     const GLogField *fields,
                     gsize n_fields,
                     gpointer user_data)
{
  if (level & TERMINAL_LOG_ERRORS)
    return g_log_writer_standard_streams (level, fields, n_fields, user_data);
  return G_LOG_WRITER_HANDLED;
}
#endif

void
_terminal_debug_init(void)
{
  g_log_set_default_handler (terminal_log_handler, NULL);
#if GLIB_CHECK_VERSION (2, 50, 0)
  g_log_set_writer_func (terminal_log_writer, NULL, NULL);
#endif

#ifdef ENABLE_DEBUG
  const GDebugKey keys[] = {
    { "accels",        TERMINAL_DEBUG_ACCELS        },
    { "clipboard",     TERMINAL_DEBUG_CLIPBOARD     },
    { "encodings",     TERMINAL_DEBUG_ENCODINGS     },
    { "server",        TERMINAL_DEBUG_SERVER        },
    { "geometry",      TERMINAL_DEBUG_GEOMETRY      },
    { "mdi",           TERMINAL_DEBUG_MDI           },
    { "processes",     TERMINAL_DEBUG_PROCESSES     },
    { "profile",       TERMINAL_DEBUG_PROFILE       },
    { "settings-list", TERMINAL_DEBUG_SETTINGS_LIST },
    { "search",        TERMINAL_DEBUG_SEARCH        },
  };

  _terminal_debug_flags = g_parse_debug_string (g_getenv ("GNOME_TERMINAL_DEBUG"),
                                                keys, G_N_ELEMENTS (keys));

#endif /* ENABLE_DEBUG */
}

