/*
 *  Copyright © 2011 Christian Persch
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

#ifndef TERMINAL_DEFINES_H
#define TERMINAL_DEFINES_H

G_BEGIN_DECLS

enum {
  _EXIT_FAILURE_WRONG_ID = 7,
  _EXIT_FAILURE_NO_UTF8 = 8,
  _EXIT_FAILURE_UNSUPPORTED_LOCALE = 9,
  _EXIT_FAILURE_GTK_INIT = 10
};

#define TERMINAL_APPLICATION_ID                 "org.gnome.Terminal"

#define TERMINAL_OBJECT_PATH_PREFIX             "/org/gnome/Terminal"
#define TERMINAL_OBJECT_INTERFACE_PREFIX        "org.gnome.Terminal"

#define TERMINAL_FACTORY_OBJECT_PATH            TERMINAL_OBJECT_PATH_PREFIX "/Factory0"
#define TERMINAL_FACTORY_INTERFACE_NAME         TERMINAL_OBJECT_INTERFACE_PREFIX ".Factory0"

#define TERMINAL_RECEIVER_OBJECT_PATH_FORMAT    TERMINAL_OBJECT_PATH_PREFIX "/screen/%s"
#define TEMRINAL_RECEIVER_INTERFACE_NAME        TERMINAL_OBJECT_INTERFACE_PREFIX ".Terminal0"

#define TERMINAL_SEARCH_PROVIDER_PATH           TERMINAL_OBJECT_PATH_PREFIX "/SearchProvider"

#define TERMINAL_ENV_SERVICE_NAME               "GNOME_TERMINAL_SERVICE"
#define TERMINAL_ENV_SCREEN                     "GNOME_TERMINAL_SCREEN"

#define TERMINAL_PORTABLE_ENV_ROOT               "GNOME_TERMINAL_PORTABLE_ROOT"
#define TERMINAL_PORTABLE_ENV_LOCALEDIR          "GNOME_TERMINAL_PORTABLE_LOCALEDIR"

/* Restore the values saved by the portable launcher, but leave its locale
 * fallback in place so commands still run in a usable UTF-8 locale. */
static inline char **
terminal_portable_restore_host_environment (char **envv)
{
  static const struct {
    const char *name;
    const char *saved_name;
  } vars[] = {
#define HOST_ENV(name) { #name, "GNOME_TERMINAL_PORTABLE_HOST_" #name }
    HOST_ENV (DBUS_SESSION_BUS_ADDRESS),
    HOST_ENV (DBUS_SESSION_BUS_PID),
    HOST_ENV (DBUS_SESSION_BUS_WINDOWID),
    HOST_ENV (DBUS_STARTER_ADDRESS),
    HOST_ENV (DBUS_STARTER_BUS_TYPE),
    HOST_ENV (DCONF_PROFILE),
    HOST_ENV (XDG_DATA_DIRS),
    HOST_ENV (GSETTINGS_SCHEMA_DIR),
#undef HOST_ENV
  };

  for (guint i = 0; i < G_N_ELEMENTS (vars); i++) {
    const char *value = g_environ_getenv (envv, vars[i].saved_name);
    envv = value != NULL ? g_environ_setenv (envv, vars[i].name, value, TRUE)
                         : g_environ_unsetenv (envv, vars[i].name);
    envv = g_environ_unsetenv (envv, vars[i].saved_name);
  }

  envv = g_environ_unsetenv (envv, TERMINAL_PORTABLE_ENV_ROOT);
  return g_environ_unsetenv (envv, TERMINAL_PORTABLE_ENV_LOCALEDIR);
}

G_END_DECLS

#endif /* !TERMINAL_DEFINES_H */
