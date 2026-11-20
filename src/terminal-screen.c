/*
 * Copyright © 2001 Havoc Pennington
 * Copyright © 2007, 2008, 2010, 2011 Christian Persch
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

#include "config.h"

#include "terminal-pcre2.h"
#include "terminal-regex.h"
#include "terminal-screen.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <uuid.h>

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
#include <sys/sysctl.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "terminal-accels.h"
#include "terminal-app.h"
#include "terminal-debug.h"
#include "terminal-defines.h"
#include "terminal-encoding.h"
#include "terminal-enums.h"
#include "terminal-intl.h"
#include "terminal-marshal.h"
#include "terminal-notebook.h"
#include "terminal-schemas.h"
#include "terminal-screen-container.h"
#include "terminal-util.h"
#include "terminal-window.h"
#include "terminal-info-bar.h"
#include "terminal-libgsystem.h"
#include "terminal-ssh.h"

#include "eggshell.h"

#define URL_MATCH_CURSOR  (GDK_HAND2)

#define SPAWN_TIMEOUT (30 * 1000 /* 30s */)

typedef struct {
  int *fd_list;
  int fd_list_len;
  const int *fd_array;
  gsize fd_array_len;
} FDSetupData;

typedef struct
{
  int tag;
  TerminalURLFlavor flavor;
} TagData;

struct _TerminalScreenPrivate
{
  char *uuid;
  gboolean registered; /* D-Bus interface is registered */

  GSettings *profile; /* never NULL */
  guint profile_changed_id;
  guint profile_forgotten_id;
  char *initial_working_directory;
  gboolean project_launch;
  gboolean confirm_direct_child;
  char *custom_title;
  char **initial_env;
  char **override_command;
  gboolean shell;
  gboolean force_shell;
  char *startup_ssh_command;
  char *startup_ssh_credential;
  char *startup_ssh_directory;
  char *ssh_login_command;
  char *reported_ssh_directory;
  char *active_ssh_command;
  char *active_ssh_credential;
  char *active_ssh_directory;
  char *active_ssh_login_command;
  gboolean track_ssh_directory;
  gboolean directory_uri_is_remote;
  gboolean keep_startup_ssh;
  int child_pid;
  GSList *match_tags;
  guint launch_child_source_id;
};

enum
{
  PROFILE_SET,
  SHOW_POPUP_MENU,
  MATCH_CLICKED,
  CLOSE_SCREEN,
  INITIAL_DIRECTORY_CHANGED,
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_PROFILE,
  PROP_ICON_TITLE,
  PROP_ICON_TITLE_SET,
  PROP_TITLE,
  PROP_INITIAL_ENVIRONMENT
};

enum
{
  TARGET_MOZ_URL,
  TARGET_NETSCAPE_URL,
  TARGET_TAB
};

static void terminal_screen_constructed (GObject             *object);
static void terminal_screen_dispose     (GObject             *object);
static void terminal_screen_finalize    (GObject             *object);
static void terminal_screen_drag_data_received (GtkWidget        *widget,
                                                GdkDragContext   *context,
                                                gint              x,
                                                gint              y,
                                                GtkSelectionData *selection_data,
                                                guint             info,
                                                guint             time);
static void terminal_screen_set_font (TerminalScreen *screen);
static void terminal_screen_system_font_changed_cb (GSettings *,
                                                    const char*,
                                                    TerminalScreen *screen);
static gboolean terminal_screen_popup_menu (GtkWidget *widget);
static gboolean terminal_screen_button_press (GtkWidget *widget,
                                              GdkEventButton *event);
static gboolean terminal_screen_do_exec (TerminalScreen *screen,
                                         FDSetupData    *data,
                                         GError **error);
static void terminal_screen_child_exited  (VteTerminal *terminal,
                                           int status);

static void terminal_screen_window_title_changed      (VteTerminal *vte_terminal,
                                                       TerminalScreen *screen);
static void terminal_screen_icon_title_changed        (VteTerminal *vte_terminal,
                                                       TerminalScreen *screen);
static void terminal_screen_current_directory_changed (TerminalScreen *screen,
                                                       gpointer user_data);
static void clear_active_ssh (TerminalScreenPrivate *priv);
#ifdef __linux__
static int get_foreground_ssh_pid (TerminalScreen *screen);
#endif

static void update_color_scheme                      (TerminalScreen *screen);

static char* terminal_screen_check_hyperlink   (TerminalScreen            *screen,
                                                GdkEvent                  *event);
static void terminal_screen_check_extra (TerminalScreen *screen,
                                         GdkEvent       *event,
                                         char           **number_info);
static char* terminal_screen_check_match       (TerminalScreen            *screen,
                                                GdkEvent                  *event,
                                                int                  *flavor);

static void terminal_screen_set_override_command (TerminalScreen  *screen,
                                                  char           **argv,
                                                  gboolean         shell);

static guint signals[LAST_SIGNAL];

typedef struct {
  const char *pattern;
  TerminalURLFlavor flavor;
} TerminalRegexPattern;

static const TerminalRegexPattern url_regex_patterns[] = {
  { REGEX_URL_AS_IS, FLAVOR_AS_IS },
  { REGEX_URL_HTTP,  FLAVOR_DEFAULT_TO_HTTP },
  { REGEX_URL_FILE,  FLAVOR_AS_IS },
  { REGEX_URL_VOIP,  FLAVOR_VOIP_CALL },
  { REGEX_EMAIL,     FLAVOR_EMAIL },
  { REGEX_NEWS_MAN,  FLAVOR_AS_IS },
};

static const TerminalRegexPattern extra_regex_patterns[] = {
  { "(0[Xx][[:xdigit:]]+|[[:digit:]]+)", FLAVOR_NUMBER },
};

static VteRegex **url_regexes;
static VteRegex **extra_regexes;
static TerminalURLFlavor *url_regex_flavors;
static TerminalURLFlavor *extra_regex_flavors;
static guint n_url_regexes;
static guint n_extra_regexes;

/* See bug #697024 */
#ifndef __linux__

#undef dup3
#define dup3 fake_dup3

static int
fake_dup3 (int fd, int fd2, int flags)
{
  if (dup2 (fd, fd2) == -1)
    return -1;

  return fcntl (fd2, F_SETFD, flags);
}
#endif /* !__linux__ */

G_DEFINE_TYPE (TerminalScreen, terminal_screen, VTE_TYPE_TERMINAL)

static void
free_tag_data (TagData *tagdata)
{
  g_slice_free (TagData, tagdata);
}

static void
precompile_regexes (const TerminalRegexPattern *regex_patterns,
                    guint n_regexes,
                    VteRegex ***regexes,
                    TerminalURLFlavor **regex_flavors)
{
  guint i;

  *regexes = g_new0 (VteRegex*, n_regexes);
  *regex_flavors = g_new0 (TerminalURLFlavor, n_regexes);

  for (i = 0; i < n_regexes; ++i)
    {
      GError *error = NULL;

      (*regexes)[i] = vte_regex_new_for_match (regex_patterns[i].pattern, -1,
                                               PCRE2_UTF | PCRE2_NO_UTF_CHECK | PCRE2_MULTILINE,
                                               &error);
      g_assert_no_error (error);

      if (!vte_regex_jit ((*regexes)[i], PCRE2_JIT_COMPLETE, &error) ||
          !vte_regex_jit ((*regexes)[i], PCRE2_JIT_PARTIAL_SOFT, &error)) {
        g_printerr ("Failed to JIT regex '%s': %s\n", regex_patterns[i].pattern, error->message);
        g_clear_error (&error);
      }

      (*regex_flavors)[i] = regex_patterns[i].flavor;
    }
}

static void
terminal_screen_class_enable_menu_bar_accel_notify_cb (GSettings *settings,
                                                       const char *key,
                                                       TerminalScreenClass *klass)
{
  static gboolean is_enabled = TRUE; /* the binding is enabled by default since GtkWidgetClass installs it */
  gboolean enable;
  GtkBindingSet *binding_set;

  enable = g_settings_get_boolean (settings, key);

  /* Only remove the 'skip' entry when we have added it previously! */
  if (enable == is_enabled)
    return;

  is_enabled = enable;

  binding_set = gtk_binding_set_by_class (klass);
  if (enable)
    gtk_binding_entry_remove (binding_set, GDK_KEY_F10, GDK_SHIFT_MASK);
  else
    gtk_binding_entry_skip (binding_set, GDK_KEY_F10, GDK_SHIFT_MASK);
}

static TerminalWindow *
terminal_screen_get_window (TerminalScreen *screen)
{
  GtkWidget *widget = GTK_WIDGET (screen);
  GtkWidget *toplevel;

  toplevel = gtk_widget_get_toplevel (widget);
  if (!gtk_widget_is_toplevel (toplevel))
    return NULL;

  return TERMINAL_WINDOW (toplevel);
}

static void
terminal_screen_realize (GtkWidget *widget)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->realize (widget);

  terminal_screen_set_font (screen);
}

static void
terminal_screen_style_updated (GtkWidget *widget)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->style_updated (widget);

  terminal_screen_set_font (screen);
}

#ifdef ENABLE_DEBUG
static void
size_request (GtkWidget *widget,
              GtkRequisition *req)
{
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                         "[screen %p] size-request %d : %d\n",
                         widget, req->width, req->height);
}

static void
size_allocate (GtkWidget *widget,
               GtkAllocation *allocation)
{
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                         "[screen %p] size-alloc   %d : %d at (%d, %d)\n",
                         widget, allocation->width, allocation->height, allocation->x, allocation->y);
}
#endif

static void
terminal_screen_init (TerminalScreen *screen)
{
  const GtkTargetEntry target_table[] = {
    { (char *) "GTK_NOTEBOOK_TAB", GTK_TARGET_SAME_APP, TARGET_TAB },
    { (char *) "text/x-moz-url",  0, TARGET_MOZ_URL },
    { (char *) "_NETSCAPE_URL", 0, TARGET_NETSCAPE_URL }
  };
  VteTerminal *terminal = VTE_TERMINAL (screen);
  TerminalScreenPrivate *priv;
  TerminalApp *app;
  GtkTargetList *target_list;
  GtkTargetEntry *targets;
  int n_targets;
  guint i;
  uuid_t u;
  char uuidstr[37];

  priv = screen->priv = G_TYPE_INSTANCE_GET_PRIVATE (screen, TERMINAL_TYPE_SCREEN, TerminalScreenPrivate);

  uuid_generate (u);
  uuid_unparse (u, uuidstr);
  priv->uuid = g_strdup (uuidstr);

  vte_terminal_set_mouse_autohide (terminal, TRUE);

  priv->child_pid = -1;

  vte_terminal_set_allow_hyperlink (terminal, TRUE);

  for (i = 0; i < n_url_regexes; ++i)
    {
      TagData *tag_data;

      tag_data = g_slice_new (TagData);
      tag_data->flavor = url_regex_flavors[i];
      tag_data->tag = vte_terminal_match_add_regex (terminal, url_regexes[i], 0);
      vte_terminal_match_set_cursor_type (terminal, tag_data->tag, URL_MATCH_CURSOR);

      priv->match_tags = g_slist_prepend (priv->match_tags, tag_data);
    }

  /* Setup DND */
  target_list = gtk_target_list_new (NULL, 0);
  gtk_target_list_add_uri_targets (target_list, 0);
  gtk_target_list_add_text_targets (target_list, 0);
  gtk_target_list_add_table (target_list, target_table, G_N_ELEMENTS (target_table));

  targets = gtk_target_table_new_from_list (target_list, &n_targets);

  gtk_drag_dest_set (GTK_WIDGET (screen),
                     GTK_DEST_DEFAULT_MOTION |
                     GTK_DEST_DEFAULT_HIGHLIGHT |
                     GTK_DEST_DEFAULT_DROP,
                     targets, n_targets,
                     GDK_ACTION_COPY | GDK_ACTION_MOVE);

  gtk_target_table_free (targets, n_targets);
  gtk_target_list_unref (target_list);

  g_signal_connect (screen, "window-title-changed",
                    G_CALLBACK (terminal_screen_window_title_changed),
                    screen);
  g_signal_connect (screen, "icon-title-changed",
                    G_CALLBACK (terminal_screen_icon_title_changed),
                    screen);
  g_signal_connect (screen, "current-directory-uri-changed",
                    G_CALLBACK (terminal_screen_current_directory_changed),
                    NULL);

  app = terminal_app_get ();
  g_signal_connect (terminal_app_get_desktop_interface_settings (app), "changed::" MONOSPACE_FONT_KEY_NAME,
                    G_CALLBACK (terminal_screen_system_font_changed_cb), screen);

#ifdef ENABLE_DEBUG
  _TERMINAL_DEBUG_IF (TERMINAL_DEBUG_GEOMETRY)
    {
      g_signal_connect_after (screen, "size-request", G_CALLBACK (size_request), NULL);
      g_signal_connect_after (screen, "size-allocate", G_CALLBACK (size_allocate), NULL);
    }
#endif
}

static void
terminal_screen_get_property (GObject *object,
                              guint prop_id,
                              GValue *value,
                              GParamSpec *pspec)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);

  switch (prop_id)
    {
      case PROP_PROFILE:
        g_value_set_object (value, terminal_screen_get_profile (screen));
        break;
      case PROP_ICON_TITLE:
        g_value_set_string (value, terminal_screen_get_icon_title (screen));
        break;
      case PROP_ICON_TITLE_SET:
        g_value_set_boolean (value, terminal_screen_get_icon_title_set (screen));
        break;
      case PROP_INITIAL_ENVIRONMENT:
        g_value_set_boxed (value, terminal_screen_get_initial_environment (screen));
        break;
      case PROP_TITLE:
        g_value_set_string (value, terminal_screen_get_title (screen));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
terminal_screen_set_property (GObject *object,
                              guint prop_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);

  switch (prop_id)
    {
      case PROP_PROFILE:
        terminal_screen_set_profile (screen, g_value_get_object (value));
        break;
      case PROP_INITIAL_ENVIRONMENT:
        terminal_screen_set_initial_environment (screen, g_value_get_boxed (value));
        break;
      case PROP_ICON_TITLE:
      case PROP_ICON_TITLE_SET:
      case PROP_TITLE:
        /* not writable */
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
terminal_screen_class_init (TerminalScreenClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  VteTerminalClass *terminal_class = VTE_TERMINAL_CLASS (klass);
  GSettings *settings;

  object_class->constructed = terminal_screen_constructed;
  object_class->dispose = terminal_screen_dispose;
  object_class->finalize = terminal_screen_finalize;
  object_class->get_property = terminal_screen_get_property;
  object_class->set_property = terminal_screen_set_property;

  widget_class->realize = terminal_screen_realize;
  widget_class->style_updated = terminal_screen_style_updated;
  widget_class->drag_data_received = terminal_screen_drag_data_received;
  widget_class->button_press_event = terminal_screen_button_press;
  widget_class->popup_menu = terminal_screen_popup_menu;

  terminal_class->child_exited = terminal_screen_child_exited;

  signals[PROFILE_SET] =
    g_signal_new (I_("profile-set"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, profile_set),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE,
                  1, G_TYPE_SETTINGS);
  
  signals[SHOW_POPUP_MENU] =
    g_signal_new (I_("show-popup-menu"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, show_popup_menu),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_POINTER);

  signals[MATCH_CLICKED] =
    g_signal_new (I_("match-clicked"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, match_clicked),
                  g_signal_accumulator_true_handled, NULL,
                  _terminal_marshal_BOOLEAN__STRING_INT_UINT,
                  G_TYPE_BOOLEAN,
                  3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT);
  
  signals[CLOSE_SCREEN] =
    g_signal_new (I_("close-screen"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, close_screen),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  signals[INITIAL_DIRECTORY_CHANGED] =
    g_signal_new (I_("initial-directory-changed"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  g_object_class_install_property
    (object_class,
     PROP_PROFILE,
     g_param_spec_object ("profile", NULL, NULL,
                          G_TYPE_SETTINGS,
                          G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property
    (object_class,
     PROP_ICON_TITLE,
     g_param_spec_string ("icon-title", NULL, NULL,
                          NULL,
                          G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property
    (object_class,
     PROP_ICON_TITLE_SET,
     g_param_spec_boolean ("icon-title-set", NULL, NULL,
                           FALSE,
                           G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property
    (object_class,
     PROP_TITLE,
     g_param_spec_string ("title", NULL, NULL,
                          NULL,
                          G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_object_class_install_property
    (object_class,
     PROP_INITIAL_ENVIRONMENT,
     g_param_spec_boxed ("initial-environment", NULL, NULL,
                         G_TYPE_STRV,
                         G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));

  g_type_class_add_private (object_class, sizeof (TerminalScreenPrivate));

  n_url_regexes = G_N_ELEMENTS (url_regex_patterns);
  precompile_regexes (url_regex_patterns, n_url_regexes, &url_regexes, &url_regex_flavors);
  n_extra_regexes = G_N_ELEMENTS (extra_regex_patterns);
  precompile_regexes (extra_regex_patterns, n_extra_regexes, &extra_regexes, &extra_regex_flavors);

  /* This fixes bug #329827 */
  settings = terminal_app_get_global_settings (terminal_app_get ());
  terminal_screen_class_enable_menu_bar_accel_notify_cb (settings, TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY, klass);
  g_signal_connect (settings, "changed::" TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY,
                    G_CALLBACK (terminal_screen_class_enable_menu_bar_accel_notify_cb), klass);
}

static void
terminal_screen_constructed (GObject *object)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);
  TerminalScreenPrivate *priv = screen->priv;

  G_OBJECT_CLASS (terminal_screen_parent_class)->constructed (object);

  terminal_app_register_screen (terminal_app_get (), screen);
  priv->registered = TRUE;
}

static void
terminal_screen_dispose (GObject *object)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);
  TerminalScreenPrivate *priv = screen->priv;
  GtkSettings *settings;

  settings = gtk_widget_get_settings (GTK_WIDGET (screen));
  g_signal_handlers_disconnect_matched (settings, G_SIGNAL_MATCH_DATA,
                                        0, 0, NULL, NULL,
                                        screen);

  if (priv->launch_child_source_id != 0)
    {
      g_source_remove (priv->launch_child_source_id);
      priv->launch_child_source_id = 0;
    }

  if (priv->registered) {
    terminal_app_unregister_screen (terminal_app_get (), screen);
    priv->registered = FALSE;
  }

  clear_active_ssh (priv);
  if (!priv->keep_startup_ssh && priv->startup_ssh_credential != NULL) {
    terminal_ssh_forget_password (priv->startup_ssh_credential);
    g_clear_pointer (&priv->startup_ssh_credential, g_free);
  }

  G_OBJECT_CLASS (terminal_screen_parent_class)->dispose (object);
}

static void
terminal_screen_finalize (GObject *object)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);
  TerminalScreenPrivate *priv = screen->priv;

  g_signal_handlers_disconnect_by_func (terminal_app_get_desktop_interface_settings (terminal_app_get ()),
                                        G_CALLBACK (terminal_screen_system_font_changed_cb),
                                        screen);

  terminal_screen_set_profile (screen, NULL);

  g_free (priv->initial_working_directory);
  g_free (priv->custom_title);
  g_strfreev (priv->override_command);
  g_strfreev (priv->initial_env);
  g_free (priv->startup_ssh_command);
  g_free (priv->startup_ssh_credential);
  g_free (priv->startup_ssh_directory);
  g_free (priv->ssh_login_command);
  g_free (priv->reported_ssh_directory);

  g_slist_foreach (priv->match_tags, (GFunc) free_tag_data, NULL);
  g_slist_free (priv->match_tags);

  g_free (priv->uuid);

  G_OBJECT_CLASS (terminal_screen_parent_class)->finalize (object);
}

TerminalScreen *
terminal_screen_new (GSettings       *profile,
                     const char      *charset,
                     char           **override_command,
                     const char      *title,
                     const char      *working_dir,
                     char           **child_env,
                     double           zoom)
{
  TerminalScreen *screen;
  TerminalScreenPrivate *priv;

  g_return_val_if_fail (G_IS_SETTINGS (profile), NULL);

  screen = g_object_new (TERMINAL_TYPE_SCREEN, NULL);
  priv = screen->priv;

  terminal_screen_set_profile (screen, profile);

  /* If we got an encoding together with an override command,
   * override the profile encoding; otherwise use the profile
   * encoding.
   */
  if (charset != NULL &&
      terminal_encodings_is_known_charset (charset) &&
      override_command != NULL) {
    vte_terminal_set_encoding (VTE_TERMINAL (screen),
                               charset,
                               NULL);
  }

  vte_terminal_set_size (VTE_TERMINAL (screen),
                         g_settings_get_int (profile, TERMINAL_PROFILE_DEFAULT_SIZE_COLUMNS_KEY),
                         g_settings_get_int (profile, TERMINAL_PROFILE_DEFAULT_SIZE_ROWS_KEY));

  /* If given an initial title, strip it of control characters and
   * feed it to the terminal.
   */
  if (title != NULL) {
    GString *seq;
    const char *p;

    seq = g_string_new ("\033]0;");
    for (p = title; *p; p = g_utf8_next_char (p)) {
      gunichar c = g_utf8_get_char (p);
      if (c < 0x20 || (c >= 0x7f && c <= 0x9f))
        continue;
      else if (c == ';')
        break;

      g_string_append_unichar (seq, c);
    }
    g_string_append (seq, "\033\\");

    vte_terminal_feed (VTE_TERMINAL (screen), seq->str, seq->len);
    g_string_free (seq, TRUE);
  }

  priv->initial_working_directory = g_strdup (working_dir);

  if (override_command)
    terminal_screen_set_override_command (screen, override_command, FALSE);
  else
    terminal_screen_set_override_command (screen, NULL, TRUE);

  if (child_env)
    terminal_screen_set_initial_environment (screen, child_env);

  vte_terminal_set_font_scale (VTE_TERMINAL (screen), zoom);
  terminal_screen_set_font (screen);

  return screen;
}

void
terminal_screen_set_project_launch (TerminalScreen *screen,
                                    gboolean confirm_direct_child)
{
  screen->priv->project_launch = TRUE;
  screen->priv->confirm_direct_child = confirm_direct_child;
}

void
terminal_screen_set_force_shell (TerminalScreen *screen)
{
  screen->priv->force_shell = TRUE;
}

const char *
terminal_screen_get_startup_ssh (TerminalScreen *screen,
                                 const char **credential)
{
  *credential = screen->priv->startup_ssh_credential;
  return screen->priv->startup_ssh_command;
}

void
terminal_screen_set_startup_ssh (TerminalScreen *screen,
                                 const char *command,
                                 const char *credential)
{
  TerminalScreenPrivate *priv = screen->priv;
  char *new_command = terminal_ssh_canonicalize_command (command);
  if (new_command == NULL && command != NULL && *command != '\0')
    new_command = g_strdup (command);
  char *new_credential = new_command != NULL && credential != NULL && *credential != '\0' ?
                         g_strdup (credential) : NULL;
  gboolean command_changed = g_strcmp0 (priv->startup_ssh_command, new_command) != 0;

  g_free (priv->startup_ssh_command);
  if (g_strcmp0 (priv->startup_ssh_credential, new_credential) != 0) {
    if (priv->startup_ssh_credential != NULL &&
        g_strcmp0 (priv->startup_ssh_credential, priv->active_ssh_credential) != 0 &&
        !priv->keep_startup_ssh)
      terminal_ssh_forget_password (priv->startup_ssh_credential);
    priv->keep_startup_ssh = FALSE;
  }
  g_free (priv->startup_ssh_credential);
  priv->startup_ssh_command = new_command;
  priv->startup_ssh_credential = new_credential;

  if (command_changed) {
    /* Directory and login command belong to the old SSH target. */
    g_clear_pointer (&priv->reported_ssh_directory, g_free);
    g_clear_pointer (&priv->startup_ssh_directory, g_free);
    g_clear_pointer (&priv->ssh_login_command, g_free);
  }
}

void
terminal_screen_keep_startup_ssh (TerminalScreen *screen)
{
  screen->priv->keep_startup_ssh = TRUE;
}

static void
release_active_ssh_credential (TerminalScreenPrivate *priv)
{
  if (priv->active_ssh_credential != NULL &&
      g_strcmp0 (priv->active_ssh_credential, priv->startup_ssh_credential) != 0)
    terminal_ssh_forget_password (priv->active_ssh_credential);
  g_clear_pointer (&priv->active_ssh_credential, g_free);
}

static void
clear_active_ssh (TerminalScreenPrivate *priv)
{
  release_active_ssh_credential (priv);
  g_clear_pointer (&priv->active_ssh_command, g_free);
  g_clear_pointer (&priv->active_ssh_directory, g_free);
  g_clear_pointer (&priv->active_ssh_login_command, g_free);
  priv->track_ssh_directory = FALSE;
}

const char *
terminal_screen_get_active_ssh (TerminalScreen *screen,
                                const char **credential,
                                const char **login_command)
{
  TerminalScreenPrivate *priv = screen->priv;
  gboolean running = priv->child_pid > 0;
  if (credential != NULL)
    *credential = running ? priv->active_ssh_credential : NULL;
  if (login_command != NULL)
    *login_command = running ? priv->active_ssh_login_command : NULL;
  return running ? priv->active_ssh_command : NULL;
}

const char *
terminal_screen_get_ssh_launch (TerminalScreen *screen,
                                const char **directory)
{
  TerminalScreenPrivate *priv = screen->priv;
  *directory = priv->active_ssh_directory;
  return priv->active_ssh_command;
}

gboolean
terminal_screen_forget_ssh_password (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  if (priv->startup_ssh_credential != NULL) {
    if (!terminal_ssh_remove_password (priv->startup_ssh_credential))
      return FALSE;
    if (g_strcmp0 (priv->startup_ssh_credential, priv->active_ssh_credential) == 0)
      g_clear_pointer (&priv->active_ssh_credential, g_free);
    g_clear_pointer (&priv->startup_ssh_credential, g_free);
  }
  if (priv->active_ssh_credential != NULL) {
    if (!terminal_ssh_remove_password (priv->active_ssh_credential))
      return FALSE;
    g_clear_pointer (&priv->active_ssh_credential, g_free);
  }
  return TRUE;
}

gboolean
terminal_screen_has_remote_session (TerminalScreen *screen)
{
  if (terminal_screen_get_active_ssh (screen, NULL, NULL) != NULL)
    return TRUE;
#ifdef __linux__
  return get_foreground_ssh_pid (screen) > 0;
#else
  return FALSE;
#endif
}

char *
terminal_screen_dup_remote_directory (TerminalScreen *screen)
{
  return g_strdup (screen->priv->reported_ssh_directory);
}

const char *
terminal_screen_get_startup_ssh_directory (TerminalScreen *screen)
{
  return screen->priv->startup_ssh_directory;
}

void
terminal_screen_set_startup_ssh_directory (TerminalScreen *screen,
                                          const char *directory)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (directory != NULL && !g_path_is_absolute (directory))
    directory = NULL;
  /* Even clearing an already empty configured path supersedes the last report.
   * A later directory notification can establish a new report. */
  g_clear_pointer (&priv->reported_ssh_directory, g_free);
  if (g_strcmp0 (priv->startup_ssh_directory, directory) == 0)
    return;
  g_free (priv->startup_ssh_directory);
  priv->startup_ssh_directory = g_strdup (directory);
}

const char *
terminal_screen_get_ssh_login_command (TerminalScreen *screen)
{
  return screen->priv->ssh_login_command;
}

void
terminal_screen_set_ssh_login_command (TerminalScreen *screen,
                                       const char *command)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (command != NULL && *command == '\0')
    command = NULL;
  if (g_strcmp0 (priv->ssh_login_command, command) == 0)
    return;

  g_free (priv->ssh_login_command);
  priv->ssh_login_command = g_strdup (command);
}

gboolean 
terminal_screen_exec (TerminalScreen *screen,
                      char          **argv,
                      char          **envv,
                      gboolean        shell,
                      const char     *cwd,
                      GUnixFDList    *fd_list,
                      GVariant       *fd_array,
                      GError        **error)
{
  TerminalScreenPrivate *priv;
  FDSetupData *data;

  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  priv = screen->priv;

  terminal_screen_set_initial_environment (screen, envv);
  terminal_screen_set_override_command (screen, argv, shell);

  if (g_strcmp0 (priv->initial_working_directory, cwd) != 0) {
    g_free (priv->initial_working_directory);
    priv->initial_working_directory = g_strdup (cwd);
    g_signal_emit (screen, signals[INITIAL_DIRECTORY_CHANGED], 0);
  }

  if (fd_list) {
    const int *fds;

    data = g_new (FDSetupData, 1);
    fds = g_unix_fd_list_peek_fds (fd_list, &data->fd_list_len);
    data->fd_list = g_memdup (fds, (data->fd_list_len + 1) * sizeof (int));
    data->fd_array = g_variant_get_fixed_array (fd_array, &data->fd_array_len, 2 * sizeof (int));
  } else
    data = NULL;

  return terminal_screen_do_exec (screen, data, error);
}

const char*
terminal_screen_get_title (TerminalScreen *screen)
{
  if (screen->priv->custom_title != NULL)
    return screen->priv->custom_title;
  return vte_terminal_get_window_title (VTE_TERMINAL (screen));
}

const char *
terminal_screen_get_custom_title (TerminalScreen *screen)
{
  return screen->priv->custom_title;
}

void
terminal_screen_set_custom_title (TerminalScreen *screen,
                                  const char *title)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (title != NULL && *title == '\0')
    title = NULL;
  if (g_strcmp0 (priv->custom_title, title) == 0)
    return;

  g_free (priv->custom_title);
  priv->custom_title = g_strdup (title);
  g_object_notify (G_OBJECT (screen), "title");
}

const char*
terminal_screen_get_icon_title (TerminalScreen *screen)
{
  return vte_terminal_get_icon_title (VTE_TERMINAL (screen));
}

gboolean
terminal_screen_get_icon_title_set (TerminalScreen *screen)
{
  return vte_terminal_get_icon_title (VTE_TERMINAL (screen)) != NULL;
}

static void
terminal_screen_profile_changed_cb (GSettings     *profile,
                                    const char    *prop_name,
                                    TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  GObject *object = G_OBJECT (screen);
  VteTerminal *vte_terminal = VTE_TERMINAL (screen);
  TerminalWindow *window;

  g_object_freeze_notify (object);

  if ((window = terminal_screen_get_window (screen)))
    {
      /* We need these in line for the set_size in
       * update_on_realize
       */
      terminal_window_update_geometry (window);
    }

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_SCROLLBAR_POLICY_KEY))
    _terminal_screen_update_scrollbar (screen);

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_ENCODING_KEY))
    {
      gs_free char *charset = g_settings_get_string (profile, TERMINAL_PROFILE_ENCODING_KEY);
      g_warn_if_fail (terminal_encodings_is_known_charset (charset));
      vte_terminal_set_encoding (vte_terminal, charset, NULL);
    }

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_CJK_UTF8_AMBIGUOUS_WIDTH_KEY))
    {
      TerminalCJKWidth width;

      width = g_settings_get_enum (profile, TERMINAL_PROFILE_CJK_UTF8_AMBIGUOUS_WIDTH_KEY);
      vte_terminal_set_cjk_ambiguous_width (vte_terminal, (int) width);
    }

  if (gtk_widget_get_realized (GTK_WIDGET (screen)) &&
      (!prop_name ||
       prop_name == I_(TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY) ||
       prop_name == I_(TERMINAL_PROFILE_FONT_KEY) ||
       prop_name == I_(TERMINAL_PROFILE_CELL_WIDTH_SCALE_KEY) ||
       prop_name == I_(TERMINAL_PROFILE_CELL_HEIGHT_SCALE_KEY)))
    terminal_screen_set_font (screen);

  if (!prop_name ||
      prop_name == I_(TERMINAL_PROFILE_BOLD_COLOR_SAME_AS_FG_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_BOLD_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_CURSOR_COLORS_SET_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_CURSOR_BACKGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_CURSOR_FOREGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_HIGHLIGHT_COLORS_SET_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_HIGHLIGHT_BACKGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_HIGHLIGHT_FOREGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_PALETTE_KEY))
    update_color_scheme (screen);

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_AUDIBLE_BELL_KEY))
      vte_terminal_set_audible_bell (vte_terminal, g_settings_get_boolean (profile, TERMINAL_PROFILE_AUDIBLE_BELL_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_SCROLL_ON_KEYSTROKE_KEY))
    vte_terminal_set_scroll_on_keystroke (vte_terminal,
                                          g_settings_get_boolean (profile, TERMINAL_PROFILE_SCROLL_ON_KEYSTROKE_KEY));
  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_SCROLL_ON_OUTPUT_KEY))
    vte_terminal_set_scroll_on_output (vte_terminal,
                                       g_settings_get_boolean (profile, TERMINAL_PROFILE_SCROLL_ON_OUTPUT_KEY));
  if (!prop_name ||
      prop_name == I_(TERMINAL_PROFILE_SCROLLBACK_LINES_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY))
    {
      glong lines = g_settings_get_boolean (profile, TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY) ?
		    -1 : g_settings_get_int (profile, TERMINAL_PROFILE_SCROLLBACK_LINES_KEY);
      vte_terminal_set_scrollback_lines (vte_terminal, lines);
    }

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_BACKSPACE_BINDING_KEY))
  vte_terminal_set_backspace_binding (vte_terminal,
                                      g_settings_get_enum (profile, TERMINAL_PROFILE_BACKSPACE_BINDING_KEY));
  
  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_DELETE_BINDING_KEY))
  vte_terminal_set_delete_binding (vte_terminal,
                                   g_settings_get_enum (profile, TERMINAL_PROFILE_DELETE_BINDING_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_ALLOW_BOLD_KEY))
    vte_terminal_set_allow_bold (vte_terminal,
                                 g_settings_get_boolean (profile, TERMINAL_PROFILE_ALLOW_BOLD_KEY));
  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_BOLD_IS_BRIGHT_KEY))
    vte_terminal_set_bold_is_bright (vte_terminal,
                                     g_settings_get_boolean (profile, TERMINAL_PROFILE_BOLD_IS_BRIGHT_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_CURSOR_BLINK_MODE_KEY))
    vte_terminal_set_cursor_blink_mode (vte_terminal,
                                        g_settings_get_enum (priv->profile, TERMINAL_PROFILE_CURSOR_BLINK_MODE_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_CURSOR_SHAPE_KEY))
    vte_terminal_set_cursor_shape (vte_terminal,
                                   g_settings_get_enum (priv->profile, TERMINAL_PROFILE_CURSOR_SHAPE_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_REWRAP_ON_RESIZE_KEY))
    vte_terminal_set_rewrap_on_resize (vte_terminal,
                                       g_settings_get_boolean (profile, TERMINAL_PROFILE_REWRAP_ON_RESIZE_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_TEXT_BLINK_MODE_KEY))
    vte_terminal_set_text_blink_mode (vte_terminal,
                                      g_settings_get_enum (profile, TERMINAL_PROFILE_TEXT_BLINK_MODE_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_WORD_CHAR_EXCEPTIONS_KEY))
    {
      gs_free char *word_char_exceptions;
      g_settings_get (profile, TERMINAL_PROFILE_WORD_CHAR_EXCEPTIONS_KEY, "ms", &word_char_exceptions);
      vte_terminal_set_word_char_exceptions (vte_terminal, word_char_exceptions);
    }

  g_object_thaw_notify (object);
}

static void
update_color_scheme (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings *profile = priv->profile;
  gs_free GdkRGBA *colors;
  gsize n_colors;
  static const GdkRGBA fg = { 0xee / 255.0, 0xee / 255.0, 0xec / 255.0, 1.0 };
  static const GdkRGBA bg = { 0x1e / 255.0, 0x1e / 255.0, 0x1e / 255.0, 1.0 };
  GdkRGBA bold;
  GdkRGBA cursor_bg, cursor_fg;
  GdkRGBA highlight_bg, highlight_fg;
  GdkRGBA *boldp;
  GdkRGBA *cursor_bgp = NULL, *cursor_fgp = NULL;
  GdkRGBA *highlight_bgp = NULL, *highlight_fgp = NULL;

  if (!g_settings_get_boolean (profile, TERMINAL_PROFILE_BOLD_COLOR_SAME_AS_FG_KEY) &&
      terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_BOLD_COLOR_KEY, &bold))
    boldp = &bold;
  else
    boldp = NULL;

  if (g_settings_get_boolean (profile, TERMINAL_PROFILE_CURSOR_COLORS_SET_KEY))
    {
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_CURSOR_BACKGROUND_COLOR_KEY, &cursor_bg))
        cursor_bgp = &cursor_bg;
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_CURSOR_FOREGROUND_COLOR_KEY, &cursor_fg))
        cursor_fgp = &cursor_fg;
    }

  if (g_settings_get_boolean (profile, TERMINAL_PROFILE_HIGHLIGHT_COLORS_SET_KEY))
    {
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_HIGHLIGHT_BACKGROUND_COLOR_KEY, &highlight_bg))
        highlight_bgp = &highlight_bg;
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_HIGHLIGHT_FOREGROUND_COLOR_KEY, &highlight_fg))
        highlight_fgp = &highlight_fg;
    }

  colors = terminal_g_settings_get_rgba_palette (profile, TERMINAL_PROFILE_PALETTE_KEY, &n_colors);
  vte_terminal_set_colors (VTE_TERMINAL (screen), &fg, &bg,
                           colors, n_colors);
  vte_terminal_set_color_bold (VTE_TERMINAL (screen), boldp);
  vte_terminal_set_color_cursor (VTE_TERMINAL (screen), cursor_bgp);
  vte_terminal_set_color_cursor_foreground (VTE_TERMINAL (screen), cursor_fgp);
  vte_terminal_set_color_highlight (VTE_TERMINAL (screen), highlight_bgp);
  vte_terminal_set_color_highlight_foreground (VTE_TERMINAL (screen), highlight_fgp);
}

static void
terminal_screen_set_font (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings *profile = priv->profile;
  PangoFontDescription *desc;
  int size;

  if (g_settings_get_boolean (profile, TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY))
    {
      desc = terminal_app_get_system_font (terminal_app_get ());
    }
  else
    {
      gs_free char *font;
      font = g_settings_get_string (profile, TERMINAL_PROFILE_FONT_KEY);
      desc = pango_font_description_from_string (font);
    }

  size = pango_font_description_get_size (desc);
  /* Sanity check */
  if (size == 0) {
    if (pango_font_description_get_size_is_absolute (desc))
      pango_font_description_set_absolute_size (desc, 10);
    else
      pango_font_description_set_size (desc, 10);
  }

  vte_terminal_set_font (VTE_TERMINAL (screen), desc);

  pango_font_description_free (desc);

  vte_terminal_set_cell_width_scale (VTE_TERMINAL (screen),
                                     g_settings_get_double (profile, TERMINAL_PROFILE_CELL_WIDTH_SCALE_KEY));
  vte_terminal_set_cell_height_scale (VTE_TERMINAL (screen),
                                      g_settings_get_double (profile, TERMINAL_PROFILE_CELL_HEIGHT_SCALE_KEY));
}

static void
terminal_screen_system_font_changed_cb (GSettings      *settings,
                                        const char     *key,
                                        TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (!gtk_widget_get_realized (GTK_WIDGET (screen)))
    return;

  if (!g_settings_get_boolean (priv->profile, TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY))
    return;

  terminal_screen_set_font (screen);
}

void
terminal_screen_set_profile (TerminalScreen *screen,
                             GSettings *profile)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings*old_profile;

  old_profile = priv->profile;
  if (profile == old_profile)
    return;

  if (priv->profile_changed_id)
    {
      g_signal_handler_disconnect (G_OBJECT (priv->profile),
                                   priv->profile_changed_id);
      priv->profile_changed_id = 0;
    }

  priv->profile = profile;
  if (profile)
    {
      g_object_ref (profile);
      priv->profile_changed_id =
        g_signal_connect (profile, "changed",
                          G_CALLBACK (terminal_screen_profile_changed_cb),
                          screen);
      terminal_screen_profile_changed_cb (profile, NULL, screen);

      g_signal_emit (G_OBJECT (screen), signals[PROFILE_SET], 0, old_profile);
    }

  if (old_profile)
    g_object_unref (old_profile);

  g_object_notify (G_OBJECT (screen), "profile");
}

GSettings*
terminal_screen_get_profile (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  return priv->profile;
}

GSettings*
terminal_screen_ref_profile (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (priv->profile != NULL)
    return g_object_ref (priv->profile);
  return NULL;
}

static void
terminal_screen_set_override_command (TerminalScreen *screen,
                                      char          **argv,
                                      gboolean        shell)
{
  TerminalScreenPrivate *priv;

  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  priv = screen->priv;
  g_strfreev (priv->override_command);
  if (argv)
    priv->override_command = g_strdupv (argv);
  else
    priv->override_command = NULL;
  priv->shell = shell;
}

void
terminal_screen_set_initial_environment (TerminalScreen *screen,
                                         char          **argv)
{
  TerminalScreenPrivate *priv;

  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  priv = screen->priv;
  g_assert (priv->initial_env == NULL);
  priv->initial_env = g_strdupv (argv);
}

char**
terminal_screen_get_initial_environment (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);

  return screen->priv->initial_env;
}

static gboolean
should_launch_ssh (TerminalScreenPrivate *priv)
{
  return priv->override_command == NULL &&
         priv->force_shell &&
         priv->startup_ssh_command != NULL;
}

static gboolean
get_child_command (TerminalScreen *screen,
                   const char     *shell_env,
                   const char     *ssh_directory,
                   GSpawnFlags    *spawn_flags_p,
                   char         ***argv_p,
                   GError        **err)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings *profile = priv->profile;
  char **argv;

  g_assert (spawn_flags_p != NULL && argv_p != NULL);

  *argv_p = argv = NULL;

  if (priv->override_command)
    {
      argv = g_strdupv (priv->override_command);

      *spawn_flags_p |= G_SPAWN_SEARCH_PATH;
    }
  else if (should_launch_ssh (priv))
    {
      argv = terminal_ssh_build_argv (priv->startup_ssh_command,
                                     priv->startup_ssh_credential,
                                     ssh_directory, priv->ssh_login_command,
                                     priv->project_launch, err);
      if (argv == NULL)
        return FALSE;
      priv->confirm_direct_child = TRUE;
    }
  else if (!priv->force_shell && g_settings_get_boolean (profile, TERMINAL_PROFILE_USE_CUSTOM_COMMAND_KEY))
    {
      gs_free char *argv_str;

      argv_str = g_settings_get_string (profile, TERMINAL_PROFILE_CUSTOM_COMMAND_KEY);
      if (!g_shell_parse_argv (argv_str, NULL, &argv, err))
        return FALSE;

      *spawn_flags_p |= G_SPAWN_SEARCH_PATH;
    }
  else if (priv->shell)
    {
      const char *only_name;
      char *shell;
      int argc = 0;

      shell = egg_shell (shell_env);

      only_name = strrchr (shell, '/');
      if (only_name != NULL)
        only_name++;
      else
        only_name = shell;

      argv = g_new (char*, 3);

      argv[argc++] = shell;

      if (g_settings_get_boolean (profile, TERMINAL_PROFILE_LOGIN_SHELL_KEY))
        argv[argc++] = g_strconcat ("-", only_name, NULL);
      else
        argv[argc++] = g_strdup (only_name);

      argv[argc++] = NULL;

      *spawn_flags_p |= G_SPAWN_FILE_AND_ARGV_ZERO;
    }

  else
    {
      g_set_error_literal (err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                           _("No command supplied nor shell requested"));
      return FALSE;
    }

  *argv_p = argv;

  return TRUE;
}

static char**
get_child_environment (TerminalScreen *screen,
                       const char *cwd,
                       char **shell)
{
  TerminalScreenPrivate *priv = screen->priv;
  char **env;
  char *e, *v;
  GHashTable *env_table;
  GHashTableIter iter;
  GPtrArray *retval;
  guint i;

  env_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  env = priv->initial_env;
  if (env)
    {
      for (i = 0; env[i]; ++i)
        {
          v = strchr (env[i], '=');
          if (v)
             g_hash_table_replace (env_table, g_strndup (env[i], v - env[i]), g_strdup (v + 1));
           else
             g_hash_table_replace (env_table, g_strdup (env[i]), NULL);
        }
    }

  g_hash_table_remove (env_table, "COLUMNS");
  g_hash_table_remove (env_table, "LINES");
  g_hash_table_remove (env_table, "GNOME_DESKTOP_ICON");

  /* WINDOWID does not work correctly ever since we don't use a native
   * GdkWindow anymore, and it also becomes incorrect if the screen is
   * moved to a different window, or the window unrealized and re-realized.
   * Additionally, it cannot ever work on non-X11 displays like wayland.
   * And on X11, the only use for this is broken foreign drawing on the
   * window (w3m etc), and trying to find the focused screen (brltty),
   * which can now be done correctly using DECSET 1004.
   * Therefore we do not set WINDOWID, and remove an existing variable.
   */
  g_hash_table_remove (env_table, "WINDOWID");

  terminal_util_add_proxy_env (env_table);

  /* These identifiers are only useful on the bus shared with the server. */
  if (g_getenv (TERMINAL_PORTABLE_ENV_ROOT) == NULL) {
    TerminalApp *app = terminal_app_get ();
    GDBusConnection *connection = g_application_get_dbus_connection (G_APPLICATION (app));
    g_hash_table_replace (env_table, g_strdup (TERMINAL_ENV_SERVICE_NAME),
                          g_strdup (g_dbus_connection_get_unique_name (connection)));
    g_hash_table_replace (env_table, g_strdup (TERMINAL_ENV_SCREEN),
                          terminal_app_dup_screen_object_path (app, screen));
  }

  /* Convert to strv */
  retval = g_ptr_array_sized_new (g_hash_table_size (env_table));
  g_hash_table_iter_init (&iter, env_table);
  while (g_hash_table_iter_next (&iter, (gpointer *) &e, (gpointer *) &v))
    g_ptr_array_add (retval, g_strdup_printf ("%s=%s", e, v ? v : ""));
  g_ptr_array_add (retval, NULL);

  *shell = g_strdup (g_hash_table_lookup (env_table, "SHELL"));

  g_hash_table_destroy (env_table);
  return (char **) g_ptr_array_free (retval, FALSE);
}

enum {
  RESPONSE_RELAUNCH,
  RESPONSE_EDIT_PREFERENCES
};

static void
info_bar_response_cb (GtkWidget *info_bar,
                      int response,
                      TerminalScreen *screen)
{
  gtk_widget_grab_focus (GTK_WIDGET (screen));

  switch (response) {
    case GTK_RESPONSE_CANCEL:
      gtk_widget_destroy (info_bar);
      g_signal_emit (screen, signals[CLOSE_SCREEN], 0);
      break;
    case RESPONSE_RELAUNCH:
      gtk_widget_destroy (info_bar);
      _terminal_screen_launch_child_on_idle (screen);
      break;
    case RESPONSE_EDIT_PREFERENCES:
      terminal_app_edit_preferences (terminal_app_get (),
                                     terminal_screen_get_profile (screen),
                                     "custom-command-entry");
      break;
    default:
      gtk_widget_destroy (info_bar);
      break;
  }
}

static void
free_fd_setup_data (FDSetupData *data)
{
  if (data == NULL)
    return;

  g_free (data->fd_list);
  g_free (data);
}

static void
terminal_screen_child_setup (FDSetupData *data)
{
  int *fds = data->fd_list;
  int n_fds = data->fd_list_len;
  const int *fd_array = data->fd_array;
  gsize fd_array_len = data->fd_array_len;
  gsize i;

  /* At this point, vte_pty_child_setup() has been called,
   * so all FDs are FD_CLOEXEC.
   */

  if (fd_array_len == 0)
    return;

  for (i = 0; i < fd_array_len; i++) {
    int target_fd = fd_array[2 * i];
    int idx = fd_array[2 * i + 1];
    int fd, r;

    g_assert (idx >= 0 && idx < n_fds);

    /* We want to move fds[idx] to target_fd */

    if (target_fd != fds[idx]) {
      int j;

      /* Need to check if @target_fd is one of the FDs in the FD list! */
      for (j = 0; j < n_fds; j++) {
        if (fds[j] == target_fd) {
          do {
            fd = fcntl (fds[j], F_DUPFD_CLOEXEC, 3);
          } while (fd == -1 && errno == EINTR);
          if (fd == -1)
            _exit (127);

          fds[j] = fd;
          break;
        }
      }
    }

    if (target_fd == fds[idx]) {
      /* Remove FD_CLOEXEC from target_fd */
      int flags;

      do {
        flags = fcntl (target_fd, F_GETFD);
      } while (flags == -1 && errno == EINTR);
      if (flags == -1)
        _exit (127);

      do {
        r = fcntl (target_fd, F_SETFD, flags & ~FD_CLOEXEC);
      } while (r == -1 && errno == EINTR);
      if (r == -1)
        _exit (127);
    } else {
      /* Now we know that target_fd can be safely overwritten. */
      errno = 0;
      do {
        fd = dup3 (fds[idx], target_fd, 0 /* no FD_CLOEXEC */);
      } while (fd == -1 && errno == EINTR);
      if (fd != target_fd)
        _exit (127);
    }

    /* Don't need to close it here since it's FD_CLOEXEC or consumed */
    fds[idx] = -1;
  }
}

static void
show_spawn_error (TerminalScreen *screen,
                  const GError *error)
{
  GtkWidget *info_bar = terminal_info_bar_new (GTK_MESSAGE_ERROR,
                                               _("_Preferences"), RESPONSE_EDIT_PREFERENCES,
                                               _("_Relaunch"), RESPONSE_RELAUNCH,
                                               NULL);
  terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                 _("There was an error creating the child process for this terminal"));
  terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                 "%s", error->message);
  g_signal_connect (info_bar, "response",
                    G_CALLBACK (info_bar_response_cb), screen);

  gtk_widget_set_halign (info_bar, GTK_ALIGN_FILL);
  gtk_widget_set_valign (info_bar, GTK_ALIGN_START);
  gtk_overlay_add_overlay (GTK_OVERLAY (terminal_screen_container_get_from_screen (screen)),
                           info_bar);
  gtk_info_bar_set_default_response (GTK_INFO_BAR (info_bar), GTK_RESPONSE_CANCEL);
  gtk_widget_show (info_bar);
}

static void
spawn_result_cb (VteTerminal *terminal,
                 GPid pid,
                 GError *error,
                 gpointer user_data)
{
  TerminalScreen *screen;
  TerminalScreenPrivate *priv;

  /* Terminal was destroyed while the spawn operation was in progress; nothing to do. */
  if (terminal == NULL)
    return;

  screen = TERMINAL_SCREEN (terminal);
  priv = screen->priv;

  priv->child_pid = pid;

  if (error) {
    clear_active_ssh (priv);
    vte_terminal_set_pty (terminal, NULL);
    show_spawn_error (screen, error);
  }
  gboolean remote = error == NULL && terminal_screen_has_remote_session (screen);
  if (remote)
    priv->directory_uri_is_remote = TRUE;
  if (remote || GPOINTER_TO_INT (user_data))
    g_signal_emit (screen, signals[INITIAL_DIRECTORY_CHANGED], 0);
}

static gboolean
terminal_screen_do_exec (TerminalScreen *screen,
                         FDSetupData    *data /* adopting */,
                         GError        **error)
{
  TerminalScreenPrivate *priv = screen->priv;
  VteTerminal *terminal = VTE_TERMINAL (screen);
  GSettings *profile;
  gs_strfreev char **env = NULL, **argv = NULL;
  gs_free char *shell = NULL;
  const char *working_dir;
  VtePtyFlags pty_flags = VTE_PTY_DEFAULT;
  GSpawnFlags spawn_flags = G_SPAWN_SEARCH_PATH_FROM_ENVP |
                            VTE_SPAWN_NO_PARENT_ENVV;
  GCancellable *cancellable = NULL;

  if (priv->child_pid != -1) {
    g_set_error_literal (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                         "Cannot launch a new child process while the terminal is still running another child process");
    free_fd_setup_data (data);
    return FALSE;
  }

  priv->launch_child_source_id = 0;

  _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                         "[screen %p] now launching the child process\n",
                         screen);

  profile = priv->profile;

  if (priv->initial_working_directory &&
      (priv->project_launch ||
       priv->force_shell ||
       priv->override_command != NULL ||
       !g_settings_get_boolean (profile, TERMINAL_PROFILE_USE_CUSTOM_COMMAND_KEY)))
    working_dir = priv->initial_working_directory;
  else
    working_dir = g_get_home_dir ();

  env = get_child_environment (screen, working_dir, &shell);

  gboolean launch_ssh = should_launch_ssh (priv);
  const char *ssh_directory = priv->reported_ssh_directory != NULL ?
                              priv->reported_ssh_directory : priv->startup_ssh_directory;
  if (!get_child_command (screen, shell, ssh_directory, &spawn_flags, &argv, error)) {
    free_fd_setup_data (data);
    return FALSE;
  }

  gboolean update_group = priv->active_ssh_command != NULL || launch_ssh;
  clear_active_ssh (priv);
  if (launch_ssh) {
    priv->active_ssh_command = g_strdup (priv->startup_ssh_command);
    priv->active_ssh_credential = g_strdup (priv->startup_ssh_credential);
    priv->active_ssh_directory = g_strdup (ssh_directory);
    priv->active_ssh_login_command = g_strdup (priv->ssh_login_command);
    priv->track_ssh_directory = terminal_ssh_command_accepts_remote_startup (priv->active_ssh_command);
    priv->directory_uri_is_remote = TRUE;
  }

  vte_terminal_spawn_async (terminal,
                            pty_flags,
                            working_dir,
                            argv,
                            env,
                            spawn_flags,
                            (GSpawnChildSetupFunc) (data ? terminal_screen_child_setup : NULL),
                            data,
                            (GDestroyNotify) (data ? free_fd_setup_data : NULL),
                            SPAWN_TIMEOUT,
                            cancellable,
                            spawn_result_cb, GINT_TO_POINTER (update_group));

  return TRUE; /* can't report any more errors since they only occur async */
}

static gboolean
terminal_screen_launch_child_cb (TerminalScreen *screen)
{
  if (should_launch_ssh (screen->priv)) {
    gs_free_error GError *error = NULL;
    if (!terminal_screen_do_exec (screen, NULL, &error))
      show_spawn_error (screen, error);
  } else {
    terminal_screen_do_exec (screen, NULL, NULL /* don't care */);
  }
  return FALSE; /* don't run again */
}

void
_terminal_screen_launch_child_on_idle (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (priv->launch_child_source_id != 0)
    return;

  _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                         "[screen %p] scheduling launching the child process on idle\n",
                         screen);

  priv->launch_child_source_id = g_idle_add ((GSourceFunc) terminal_screen_launch_child_cb, screen);
}

static TerminalScreenPopupInfo *
terminal_screen_popup_info_new (TerminalScreen *screen)
{
  TerminalScreenPopupInfo *info;

  info = g_slice_new0 (TerminalScreenPopupInfo);
  info->ref_count = 1;

  return info;
}

TerminalScreenPopupInfo *
terminal_screen_popup_info_ref (TerminalScreenPopupInfo *info)
{
  g_return_val_if_fail (info != NULL, NULL);

  info->ref_count++;
  return info;
}

void
terminal_screen_popup_info_unref (TerminalScreenPopupInfo *info)
{
  g_return_if_fail (info != NULL);

  if (--info->ref_count > 0)
    return;

  g_free (info->hyperlink);
  g_free (info->url);
  g_free (info->number_info);
  g_slice_free (TerminalScreenPopupInfo, info);
}

static gboolean
terminal_screen_popup_menu (GtkWidget *widget)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);
  TerminalScreenPopupInfo *info;

  info = terminal_screen_popup_info_new (screen);
  info->button = 0;
  info->timestamp = gtk_get_current_event_time ();

  g_signal_emit (screen, signals[SHOW_POPUP_MENU], 0, info);
  terminal_screen_popup_info_unref (info);

  return TRUE;
}

static void
terminal_screen_do_popup (TerminalScreen *screen,
                          GdkEventButton *event,
                          char *hyperlink,
                          char *url,
                          int url_flavor,
                          char *number_info)
{
  TerminalScreenPopupInfo *info;

  info = terminal_screen_popup_info_new (screen);
  info->button = event->button;
  info->state = event->state & gtk_accelerator_get_default_mod_mask ();
  info->timestamp = event->time;
  info->hyperlink = hyperlink; /* adopted */
  info->url = url; /* adopted */
  info->url_flavor = url_flavor;
  info->number_info = number_info; /* adopted */

  g_signal_emit (screen, signals[SHOW_POPUP_MENU], 0, info);
  terminal_screen_popup_info_unref (info);
}

static gboolean
terminal_screen_button_press (GtkWidget      *widget,
                              GdkEventButton *event)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);
  gboolean (* button_press_event) (GtkWidget*, GdkEventButton*) =
    GTK_WIDGET_CLASS (terminal_screen_parent_class)->button_press_event;
  gs_free char *hyperlink = NULL;
  gs_free char *url = NULL;
  int url_flavor = 0;
  gs_free char *number_info = NULL;
  guint state;

  state = event->state & gtk_accelerator_get_default_mod_mask ();

  if (event->type == GDK_BUTTON_PRESS && terminal_notebook_get_peer (screen) != NULL)
    gtk_widget_grab_focus (widget);

  hyperlink = terminal_screen_check_hyperlink (screen, (GdkEvent*)event);
  url = terminal_screen_check_match (screen, (GdkEvent*)event, &url_flavor);
  terminal_screen_check_extra (screen, (GdkEvent*)event, &number_info);

  if (hyperlink != NULL &&
      (event->button == 1 || event->button == 2) &&
      (state & GDK_CONTROL_MASK))
    {
      gboolean handled = FALSE;

      g_signal_emit (screen, signals[MATCH_CLICKED], 0,
                     hyperlink,
                     FLAVOR_AS_IS,
                     state,
                     &handled);
      if (handled)
        return TRUE; /* don't do anything else such as select with the click */
    }

  if (url != NULL &&
      (event->button == 1 || event->button == 2) &&
      (state & GDK_CONTROL_MASK))
    {
      gboolean handled = FALSE;

      g_signal_emit (screen, signals[MATCH_CLICKED], 0,
                     url,
                     url_flavor,
                     state,
                     &handled);
      if (handled)
        return TRUE; /* don't do anything else such as select with the click */
    }

  if (event->type == GDK_BUTTON_PRESS && event->button == 3)
    {
      if (!(event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK)))
        {
          /* on right-click, we should first try to send the mouse event to
           * the client, and popup only if that's not handled. */
          if (button_press_event && button_press_event (widget, event))
            return TRUE;

          terminal_screen_do_popup (screen, event, hyperlink, url, url_flavor, number_info);
          hyperlink = NULL; /* adopted to the popup info */
          url = NULL; /* ditto */
          number_info = NULL; /* ditto */
          return TRUE;
        }
      else if (!(event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)))
        {
          /* do popup on shift+right-click */
          terminal_screen_do_popup (screen, event, hyperlink, url, url_flavor, number_info);
          hyperlink = NULL; /* adopted to the popup info */
          url = NULL; /* ditto */
          number_info = NULL; /* ditto */
          return TRUE;
        }
    }

  /* default behavior is to let the terminal widget deal with it */
  if (button_press_event)
    return button_press_event (widget, event);

  return FALSE;
}

const char *
terminal_screen_get_initial_directory (TerminalScreen *screen)
{
  return screen->priv->initial_working_directory;
}

#ifdef __linux__
static int
get_pty_foreground_pid (TerminalScreen *screen)
{
  VtePty *pty = vte_terminal_get_pty (VTE_TERMINAL (screen));

  return pty != NULL ? tcgetpgrp (vte_pty_get_fd (pty)) : -1;
}

static int
get_foreground_ssh_pid (TerminalScreen *screen)
{
  if (screen->priv->active_ssh_command != NULL)
    return -1;
  int pid = get_pty_foreground_pid (screen);
  if (pid <= 0)
    return -1;

  char filename[64];
  g_snprintf (filename, sizeof filename, "/proc/%d/exe", pid);
  gs_free char *executable = g_file_read_link (filename, NULL);
  return executable != NULL && g_str_has_suffix (executable, "/ssh") ? pid : -1;
}
#endif

char *
terminal_screen_dup_foreground_ssh (TerminalScreen *screen)
{
#ifdef __linux__
  int pid = get_foreground_ssh_pid (screen);
  if (pid <= 0)
    return NULL;

  char filename[64];
  gs_free char *cmdline = NULL;
  gsize length;
  g_snprintf (filename, sizeof filename, "/proc/%d/cmdline", pid);
  if (!g_file_get_contents (filename, &cmdline, &length, NULL) ||
      length == 0 || cmdline[length - 1] != '\0' || pid != get_foreground_ssh_pid (screen))
    return NULL;

  const char *end = cmdline + length;
  gsize argc = 0;
  for (const char *arg = cmdline; arg < end; arg += strlen (arg) + 1)
    ++argc;

  gs_free char **argv = g_new (char *, argc + 1);
  char *arg = cmdline;
  for (gsize i = 0; i < argc; ++i) {
    argv[i] = arg;
    arg += strlen (arg) + 1;
  }
  argv[argc] = NULL;
  return terminal_ssh_join_command (argv);
#else
  return NULL;
#endif
}

static void
terminal_screen_current_directory_changed (TerminalScreen *screen,
                                           gpointer user_data)
{
  TerminalScreenPrivate *priv = screen->priv;

  gboolean managed = priv->active_ssh_command != NULL;
  gboolean remote = managed || terminal_screen_has_remote_session (screen);
  gboolean changed = priv->directory_uri_is_remote != remote;
  priv->directory_uri_is_remote = remote;
  if (!managed && changed)
    g_signal_emit (screen, signals[INITIAL_DIRECTORY_CHANGED], 0);

  /* Only a new OSC 7 report from the managed child's lifetime is remote.
   * VTE retains the previous child's URI; the hostname alone proves nothing. */
  if (!priv->track_ssh_directory ||
      g_strcmp0 (priv->active_ssh_command, priv->startup_ssh_command) != 0)
    return;

  const char *uri = vte_terminal_get_current_directory_uri (VTE_TERMINAL (screen));
  gs_free char *path = uri != NULL ? g_filename_from_uri (uri, NULL, NULL) : NULL;
  if (path == NULL || !g_path_is_absolute (path) ||
      g_strcmp0 (priv->reported_ssh_directory, path) == 0)
    return;

  g_free (priv->reported_ssh_directory);
  priv->reported_ssh_directory = g_steal_pointer (&path);
}

/**
 * terminal_screen_get_current_dir:
 * @screen:
 *
 * Uses the terminal's reported local working directory when available.
 * On Linux, falls back to the foreground process and then the primary child.
 * 
 * Returns: a newly allocated string containing the current working directory,
 *   or %NULL on failure
 */
char *
terminal_screen_get_current_dir (TerminalScreen *screen)
{
  const char *uri;

  if (screen->priv->active_ssh_command != NULL || terminal_screen_has_remote_session (screen))
    return NULL;

  uri = !screen->priv->directory_uri_is_remote ?
        vte_terminal_get_current_directory_uri (VTE_TERMINAL (screen)) : NULL;
  if (uri != NULL) {
    gs_free char *hostname = NULL;
    char *path = g_filename_from_uri (uri, &hostname, NULL);

    if (hostname == NULL || g_ascii_strcasecmp (hostname, "localhost") == 0 ||
        g_ascii_strcasecmp (hostname, g_get_host_name ()) == 0)
      return path;
    g_free (path);
    return NULL;
  }

#ifdef __linux__
  int foreground = get_pty_foreground_pid (screen);
  int child = screen->priv->child_pid;
  char filename[64];

  if (foreground > 0 && foreground != child) {
    g_snprintf (filename, sizeof filename, "/proc/%d/cwd", foreground);
    char *path = g_file_read_link (filename, NULL);
    if (path != NULL)
      return path;
  }

  if (child > 0) {
    g_snprintf (filename, sizeof filename, "/proc/%d/cwd", child);
    return g_file_read_link (filename, NULL);
  }
#endif

  return NULL;
}

static void
terminal_screen_window_title_changed (VteTerminal *vte_terminal,
                                      TerminalScreen *screen)
{
  if (screen->priv->custom_title == NULL)
    g_object_notify (G_OBJECT (screen), "title");
}

static void
terminal_screen_icon_title_changed (VteTerminal *vte_terminal,
                                    TerminalScreen *screen)
{
  g_object_notify (G_OBJECT (screen), "icon-title");
  g_object_notify (G_OBJECT (screen), "icon-title-set");
}

static void
terminal_screen_child_exited (VteTerminal *terminal,
                              int status)
{
  TerminalScreen *screen = TERMINAL_SCREEN (terminal);
  TerminalScreenPrivate *priv = screen->priv;
  TerminalExitAction action;

  /* No need to chain up to VteTerminalClass::child_exited since it's NULL */

  _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                         "[screen %p] child process exited\n",
                         screen);

  gboolean managed = priv->active_ssh_command != NULL;
  priv->child_pid = -1;
  priv->track_ssh_directory = FALSE;
  release_active_ssh_credential (priv);
  g_clear_pointer (&priv->active_ssh_login_command, g_free);
  if (gtk_widget_in_destruction (GTK_WIDGET (screen)))
    return;
  if (!managed && priv->directory_uri_is_remote)
    g_signal_emit (screen, signals[INITIAL_DIRECTORY_CHANGED], 0);
  
  action = g_settings_get_enum (priv->profile, TERMINAL_PROFILE_EXIT_ACTION_KEY);

  /* Keep failed SSH connections visible; never reconnect in an automatic loop. */
  if (managed)
    action = TERMINAL_EXIT_HOLD;
  /* Shell exit isn't a request to close the other shell in the same tab. */
  else if (action == TERMINAL_EXIT_CLOSE && terminal_notebook_get_peer (screen) != NULL)
    action = TERMINAL_EXIT_HOLD;
  
  switch (action)
    {
    case TERMINAL_EXIT_CLOSE:
      g_signal_emit (screen, signals[CLOSE_SCREEN], 0);
      break;
    case TERMINAL_EXIT_RESTART:
      _terminal_screen_launch_child_on_idle (screen);
      break;
    case TERMINAL_EXIT_HOLD: {
      GtkWidget *info_bar;

      info_bar = terminal_info_bar_new (GTK_MESSAGE_INFO,
                                        _("_Relaunch"), RESPONSE_RELAUNCH,
                                        NULL);
      if (WIFEXITED (status)) {
        terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                      _("The child process exited normally with status %d."), WEXITSTATUS (status));
      } else if (WIFSIGNALED (status)) {
        terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                      _("The child process was aborted by signal %d."), WTERMSIG (status));
      } else {
        terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                      _("The child process was aborted."));
      }
      g_signal_connect (info_bar, "response",
                        G_CALLBACK (info_bar_response_cb), screen);

      gtk_widget_set_halign (info_bar, GTK_ALIGN_FILL);
      gtk_widget_set_valign (info_bar, GTK_ALIGN_START);
      gtk_overlay_add_overlay (GTK_OVERLAY (terminal_screen_container_get_from_screen (screen)),
                               info_bar);
      gtk_info_bar_set_default_response (GTK_INFO_BAR (info_bar), RESPONSE_RELAUNCH);
      gtk_widget_show (info_bar);
      break;
    }

    default:
      break;
    }
}

static void
terminal_screen_drag_data_received (GtkWidget        *widget,
                                    GdkDragContext   *context,
                                    gint              x,
                                    gint              y,
                                    GtkSelectionData *selection_data,
                                    guint             info,
                                    guint             timestamp)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);
  const guchar *selection_data_data;
  GdkAtom selection_data_target;
  gint selection_data_length, selection_data_format;

  selection_data_data = gtk_selection_data_get_data (selection_data);
  selection_data_target = gtk_selection_data_get_target (selection_data);
  selection_data_length = gtk_selection_data_get_length (selection_data);
  selection_data_format = gtk_selection_data_get_format (selection_data);

#if 0
  {
    GList *tmp;

    g_print ("info: %d\n", info);
    tmp = context->targets;
    while (tmp != NULL)
      {
        GdkAtom atom = GDK_POINTER_TO_ATOM (tmp->data);

        g_print ("Target: %s\n", gdk_atom_name (atom));        
        
        tmp = tmp->next;
      }

    g_print ("Chosen target: %s\n", gdk_atom_name (selection_data->target));
  }
#endif

  if (gtk_targets_include_uri (&selection_data_target, 1))
    {
      gs_strfreev char **uris;
      gs_free char *text = NULL;
      gsize len;

      uris = gtk_selection_data_get_uris (selection_data);
      if (!uris)
        return;

      terminal_util_transform_uris_to_quoted_fuse_paths (uris);

      text = terminal_util_concat_uris (uris, &len);
      vte_terminal_feed_child (VTE_TERMINAL (screen), text, len);
    }
  else if (gtk_targets_include_text (&selection_data_target, 1))
    {
      gs_free char *text;

      text = (char *) gtk_selection_data_get_text (selection_data);
      if (text && text[0])
        vte_terminal_feed_child (VTE_TERMINAL (screen), text, strlen (text));
    }
  else switch (info)
    {
    case TARGET_MOZ_URL:
      {
        char *utf8_data, *newline, *text;
        char *uris[2];
        gsize len;
        
        /* MOZ_URL is in UCS-2 but in format 8. BROKEN!
         *
         * The data contains the URL, a \n, then the
         * title of the web page.
         */
        if (selection_data_format != 8 ||
            selection_data_length == 0 ||
            (selection_data_length % 2) != 0)
          return;

        utf8_data = g_utf16_to_utf8 ((const gunichar2*) selection_data_data,
                                     selection_data_length / 2,
                                     NULL, NULL, NULL);
        if (!utf8_data)
          return;

        newline = strchr (utf8_data, '\n');
        if (newline)
          *newline = '\0';

        uris[0] = utf8_data;
        uris[1] = NULL;
        terminal_util_transform_uris_to_quoted_fuse_paths (uris); /* This may replace uris[0] */

        text = terminal_util_concat_uris (uris, &len);
        vte_terminal_feed_child (VTE_TERMINAL (screen), text, len);
        g_free (text);
        g_free (uris[0]);
      }
      break;

    case TARGET_NETSCAPE_URL:
      {
        char *utf8_data, *newline, *text;
        char *uris[2];
        gsize len;
        
        /* The data contains the URL, a \n, then the
         * title of the web page.
         */
        if (selection_data_length < 0 || selection_data_format != 8)
          return;

        utf8_data = g_strndup ((char *) selection_data_data, selection_data_length);
        newline = strchr (utf8_data, '\n');
        if (newline)
          *newline = '\0';

        uris[0] = utf8_data;
        uris[1] = NULL;
        terminal_util_transform_uris_to_quoted_fuse_paths (uris); /* This may replace uris[0] */

        text = terminal_util_concat_uris (uris, &len);
        vte_terminal_feed_child (VTE_TERMINAL (screen), text, len);
        g_free (text);
        g_free (uris[0]);
      }
      break;

    case TARGET_TAB:
      {
        GtkWidget *container;
        TerminalScreen *moving_screen;
        TerminalWindow *source_window;
        TerminalWindow *dest_window;

        container = *(GtkWidget**) selection_data_data;
        if (!GTK_IS_WIDGET (container))
          return;

        moving_screen = terminal_notebook_get_page_screen (container);
        if (!TERMINAL_IS_SCREEN (moving_screen))
          return;

        source_window = terminal_screen_get_window (moving_screen);
        dest_window = terminal_screen_get_window (screen);
        terminal_window_move_screen (source_window, dest_window, moving_screen, -1);

        gtk_drag_finish (context, TRUE, TRUE, timestamp);
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

void
_terminal_screen_update_scrollbar (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  TerminalScreenContainer *container;
  GtkPolicyType vpolicy;

  container = terminal_screen_container_get_from_screen (screen);
  if (container == NULL)
    return;

  vpolicy = g_settings_get_enum (priv->profile, TERMINAL_PROFILE_SCROLLBAR_POLICY_KEY);

  terminal_screen_container_set_policy (container, GTK_POLICY_NEVER, vpolicy);
}

void
terminal_screen_get_size (TerminalScreen *screen,
			  int       *width_chars,
			  int       *height_chars)
{
  VteTerminal *terminal = VTE_TERMINAL (screen);

  *width_chars = vte_terminal_get_column_count (terminal);
  *height_chars = vte_terminal_get_row_count (terminal);
}

void
terminal_screen_get_cell_size (TerminalScreen *screen,
			       int                  *cell_width_pixels,
			       int                  *cell_height_pixels)
{
  VteTerminal *terminal = VTE_TERMINAL (screen);

  *cell_width_pixels = vte_terminal_get_char_width (terminal);
  *cell_height_pixels = vte_terminal_get_char_height (terminal);
}

static char*
terminal_screen_check_hyperlink (TerminalScreen *screen,
                                 GdkEvent       *event)
{
  return vte_terminal_hyperlink_check_event (VTE_TERMINAL (screen), event);
}

static char*
terminal_screen_check_match (TerminalScreen *screen,
                             GdkEvent       *event,
                             int       *flavor)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSList *tags;
  int tag;
  char *match;

  match = vte_terminal_match_check_event (VTE_TERMINAL (screen), event, &tag);
  for (tags = priv->match_tags; tags != NULL; tags = tags->next)
    {
      TagData *tag_data = (TagData*) tags->data;
      if (tag_data->tag == tag)
	{
	  if (flavor)
	    *flavor = tag_data->flavor;
	  return match;
	}
    }

  g_free (match);
  return NULL;
}

static void
terminal_screen_check_extra (TerminalScreen *screen,
                             GdkEvent       *event,
                             char           **number_info)
{
  guint i;
  char **matches;
  gboolean flavor_number_found = FALSE;

  matches = g_newa (char *, n_extra_regexes);
  memset(matches, 0, sizeof(char*) * n_extra_regexes);

  if (
      vte_terminal_event_check_regex_simple (VTE_TERMINAL (screen),
                                             event,
                                             extra_regexes,
                                             n_extra_regexes,
                                             0,
                                             matches))
    {
      for (i = 0; i < n_extra_regexes; i++)
        {
          if (matches[i] != NULL)
            {
              /* Store the first match for each flavor, free all the others */
              switch (extra_regex_flavors[i])
                {
                case FLAVOR_NUMBER:
                  if (!flavor_number_found)
                    {
                      *number_info = terminal_util_number_info (matches[i]);
                      flavor_number_found = TRUE;
                    }
                  g_free (matches[i]);
                  break;
                default:
                  g_free (matches[i]);
                }
            }
        }
    }
}

/**
 * terminal_screen_has_foreground_process:
 * @screen:
 * @process_name: (out) (allow-none): the basename of the program, or %NULL
 * @cmdline: (out) (allow-none): the full command line, or %NULL
 *
 * Checks whether there's a foreground process other than the primary child
 * or a running direct child that requires close confirmation.
 * 
 * Returns: %TRUE iff such a process is running in @screen
 */
gboolean
terminal_screen_has_foreground_process (TerminalScreen *screen,
                                        char           **process_name,
                                        char           **cmdline)
{
  TerminalScreenPrivate *priv = screen->priv;
  gs_free char *command = NULL;
  gs_free char *data_buf = NULL;
  gs_free char *basename = NULL;
  gs_free char *name = NULL;
  VtePty *pty;
  int fd;
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
  int mib[4];
#else
  char filename[64];
#endif
  char *data;
  gsize i;
  gsize len;
  int fgpid;

  if (priv->child_pid == -1)
    return FALSE;

  if (priv->confirm_direct_child) {
    fgpid = priv->child_pid;
  } else {
    pty = vte_terminal_get_pty (VTE_TERMINAL (screen));
    if (pty == NULL)
      return FALSE;

    fd = vte_pty_get_fd (pty);
    if (fd == -1)
      return FALSE;

    fgpid = tcgetpgrp (fd);
    if (fgpid == -1 || fgpid == priv->child_pid)
      return FALSE;
  }

  if (!process_name && !cmdline)
    return TRUE;

#if defined(__FreeBSD__) || defined(__DragonFly__)
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_ARGS;
  mib[3] = fgpid;
  if (sysctl (mib, G_N_ELEMENTS (mib), NULL, &len, NULL, 0) == -1)
      return TRUE;

  data_buf = g_malloc0 (len);
  if (sysctl (mib, G_N_ELEMENTS (mib), data_buf, &len, NULL, 0) == -1)
      return TRUE;
  data = data_buf;
#elif defined(__OpenBSD__)
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC_ARGS;
  mib[2] = fgpid;
  mib[3] = KERN_PROC_ARGV;
  if (sysctl (mib, G_N_ELEMENTS (mib), NULL, &len, NULL, 0) == -1)
      return TRUE;

  data_buf = g_malloc0 (len);
  if (sysctl (mib, G_N_ELEMENTS (mib), data_buf, &len, NULL, 0) == -1)
      return TRUE;
  data = ((char**)data_buf)[0];
#else
  g_snprintf (filename, sizeof (filename), "/proc/%d/cmdline", fgpid);
  if (!g_file_get_contents (filename, &data_buf, &len, NULL))
    return TRUE;
  data = data_buf;
#endif

  basename = g_path_get_basename (data);
  if (!basename)
    return TRUE;

  name = g_filename_to_utf8 (basename, -1, NULL, NULL, NULL);
  if (!name)
    return TRUE;

  if (process_name)
    gs_transfer_out_value (process_name, &name);

  if (len > 0 && data[len - 1] == '\0')
    len--;
  for (i = 0; i < len; i++)
    {
      if (data[i] == '\0')
        data[i] = ' ';
    }

  command = g_filename_to_utf8 (data, -1, NULL, NULL, NULL);
  if (!command)
    return TRUE;

  if (cmdline)
    gs_transfer_out_value (cmdline, &command);

  return TRUE;
}

const char *
terminal_screen_get_uuid (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);

  return screen->priv->uuid;
}
