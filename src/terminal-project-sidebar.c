/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "config.h"

#include <glib/gi18n.h>
#include <string.h>

#include "terminal-app.h"
#include "terminal-libgsystem.h"
#include "terminal-notebook.h"
#include "terminal-project-sidebar.h"
#include "terminal-schemas.h"
#include "terminal-ssh.h"

#define PROJECT_TERMINAL_INDENT 36
#define REMOTE_PROJECT_URI_PREFIX "ssh-project://"
#define DEFAULT_PROJECT_ID "default"
#define SCREEN_KEY "terminal-project-screen"
#define DIRECTORY_KEY "terminal-project-directory"
#define PROJECT_ROW_KEY "terminal-project-row"
#define PROJECT_NAME_KEY "terminal-project-search-name"
#define PROJECT_TOGGLE_KEY "terminal-project-toggle"
#define PROJECT_TAIL_KEY "terminal-project-tail"

typedef struct {
  TerminalWindow *window;
  TerminalMdiContainer *container;
  GSettings *settings;
  GHashTable *collapsed;
  GtkListBoxRow *active_project;
  GtkListBoxRow *search_target;
  GtkWidget *search;
  GtkWidget *bookmark_notice;
  GtkWidget *bookmark_status;
  GtkScrolledWindow *scroll;
  char *search_query;
} ProjectSidebar;

typedef struct {
  const char *command;
  const char *credential;
  const char *login_command;
} SshProject;

typedef enum {
  PROJECT_UPDATE_OK,
  PROJECT_UPDATE_DUPLICATE,
  PROJECT_UPDATE_FAILED
} ProjectUpdate;

static void select_terminal (GtkListBox *list, TerminalScreen *screen);
static void choose_directory (GtkListBox *list, const char *old_uri);
static ProjectUpdate update_project_directory (GtkListBox *list,
                                               const char *old_uri,
                                               const char *uri,
                                               const SshProject *ssh);

static void
free_sidebar (gpointer data)
{
  ProjectSidebar *sidebar = data;

  g_hash_table_unref (sidebar->collapsed);
  g_free (sidebar->search_query);
  g_free (sidebar);
}

static ProjectSidebar *
get_sidebar (GtkListBox *list)
{
  return g_object_get_data (G_OBJECT (list), "terminal-project-sidebar");
}

static GtkListBox *
get_row_list (GtkListBoxRow *row)
{
  return GTK_LIST_BOX (gtk_widget_get_parent (GTK_WIDGET (row)));
}

static GtkListBoxRow *
get_project_row (GtkListBoxRow *row)
{
  if (g_object_get_data (G_OBJECT (row), PROJECT_NAME_KEY) != NULL)
    return row;
  return g_object_get_data (G_OBJECT (row), PROJECT_ROW_KEY);
}

static const char *
project_id (GtkListBoxRow *row)
{
  const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);

  return uri != NULL ? uri : DEFAULT_PROJECT_ID;
}

static void
mark_row (GtkListBoxRow **current,
          GtkListBoxRow *row,
          const char *style_class)
{
  if (*current == row)
    return;
  if (*current != NULL)
    gtk_style_context_remove_class (gtk_widget_get_style_context (GTK_WIDGET (*current)), style_class);
  *current = row;
  if (row != NULL)
    gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (row)), style_class);
}

static void
show_project_message (TerminalWindow *window,
                      GtkMessageType type,
                      const char *message,
                      const char *detail)
{
  GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                             GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                             type, GTK_BUTTONS_CLOSE,
                                             "%s", message);
  if (detail != NULL)
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", detail);
  g_signal_connect_swapped (dialog, "response", G_CALLBACK (gtk_widget_destroy), dialog);
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
show_error (TerminalWindow *window,
            const char *message,
            const char *detail)
{
  show_project_message (window, GTK_MESSAGE_ERROR, message, detail);
}

static gboolean
is_remote_project (const char *uri)
{
  return uri != NULL && g_str_has_prefix (uri, REMOTE_PROJECT_URI_PREFIX);
}

/* POSIX leaves exactly two leading slashes implementation-defined. Do not
 * resolve "..": a component before it might be a symlink on the remote host. */
static char *
normalize_remote_path (const char *path)
{
  GString *normalized = g_string_new ("/");
  if (path[1] == '/' && path[2] != '/')
    g_string_append_c (normalized, '/');
  gsize root_length = normalized->len;

  while (*(path += strspn (path, "/")) != '\0') {
    gsize length = strcspn (path, "/");
    if (length != 1 || *path != '.') {
      if (normalized->len > root_length)
        g_string_append_c (normalized, '/');
      g_string_append_len (normalized, path, length);
    }
    path += length;
  }
  return g_string_free (normalized, FALSE);
}

static char *
remote_project_uri (const char *command,
                    const char *path)
{
  if (command == NULL || *command == '\0' || path == NULL || path[0] != '/')
    return NULL;

  gs_free char *canonical = terminal_ssh_canonicalize_command (command);
  if (canonical == NULL)
    return NULL;
  gs_free char *host = g_compute_checksum_for_string (G_CHECKSUM_SHA256, canonical, -1);
  gs_free char *normalized = normalize_remote_path (path);
  gs_free char *escaped = g_uri_escape_string (normalized, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
  return g_strconcat (REMOTE_PROJECT_URI_PREFIX, host, escaped, NULL);
}

static char *
dup_remote_project_path (const char *uri)
{
  const char *path = strchr (uri + sizeof REMOTE_PROJECT_URI_PREFIX - 1, '/');

  return path != NULL ? g_uri_unescape_string (path, NULL) : NULL;
}

/* Storage and UI still use the original URI, including its collapse/order
 * entry. Derive a separate comparison key from the stored command and path. */
static char *
saved_remote_project_key (GVariant *projects,
                          const char *uri)
{
  const char *command, *credential, *login_command;
  if (!is_remote_project (uri) ||
      !g_variant_lookup (projects, uri, "(&s&s&s)", &command, &credential, &login_command))
    return NULL;

  gs_free char *path = dup_remote_project_path (uri);
  return remote_project_uri (command, path);
}

static GtkListBoxRow *
find_remote_project (GHashTable *projects,
                     const char *command,
                     const char *path)
{
  gs_free char *uri = remote_project_uri (command, path);
  if (uri == NULL)
    return NULL;
  char *root = strchr (uri + sizeof REMOTE_PROJECT_URI_PREFIX - 1, '/');
  gsize root_length = root[1] == '/' ? 2 : 1;
  char *end = root + strlen (root);

  for (;;) {
    GtkListBoxRow *project = g_hash_table_lookup (projects, uri);
    if (project != NULL)
      return project;
    if (end == root + root_length)
      return NULL;
    char *slash = strrchr (root, '/');
    /* The parent of a path ending in ".." is not its textual prefix. */
    if (g_str_equal (slash + 1, ".."))
      return NULL;
    end = MAX (slash, root + root_length);
    *end = '\0';
  }
}

static void
align_label_start (GtkWidget *label)
{
#if GTK_CHECK_VERSION (3, 16, 0)
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
#else
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
#endif
}

static GtkWidget *
new_label (const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  align_label_start (label);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  return label;
}

static void
show_bookmark_status (ProjectSidebar *sidebar,
                      const char *path)
{
  gs_free char *name = g_filename_display_basename (path);
  gs_free char *message = g_strdup_printf (_("Project added: %s"), name);
  gs_free char *display_path = g_filename_display_name (path);

  gtk_label_set_text (GTK_LABEL (sidebar->bookmark_status), message);
  gtk_widget_set_tooltip_text (sidebar->bookmark_status, display_path);
  gtk_widget_show (sidebar->bookmark_notice);
}

static GtkWidget *
new_options_button (GtkWidget *menu,
                    const char *name)
{
  GtkWidget *button = gtk_menu_button_new ();

  gtk_button_set_image (GTK_BUTTON (button),
                        gtk_image_new_from_icon_name ("view-more-symbolic", GTK_ICON_SIZE_MENU));
  gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text (button, name);
  atk_object_set_name (gtk_widget_get_accessible (button), name);
  gtk_widget_show_all (menu);
  gtk_menu_button_set_popup (GTK_MENU_BUTTON (button), menu);
  return button;
}

static gboolean
filter_sidebar_row (GtkListBoxRow *row,
                    gpointer user_data)
{
  ProjectSidebar *sidebar = user_data;
  GtkListBoxRow *project = get_project_row (row);

  if (project == NULL)
    return sidebar->search_query == NULL;
  if (sidebar->search_query != NULL &&
      strstr (g_object_get_data (G_OBJECT (project), PROJECT_NAME_KEY), sidebar->search_query) == NULL)
    return FALSE;
  if (row == project)
    return TRUE;

  return !g_hash_table_contains (sidebar->collapsed, project_id (project));
}

static void
search_projects (GtkSearchEntry *entry,
                 GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_free char *text = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));

  mark_row (&sidebar->search_target, NULL, "project-search-target");
  g_strstrip (text);
  g_free (sidebar->search_query);
  sidebar->search_query = *text != '\0' ? g_utf8_casefold (text, -1) : NULL;
  gtk_list_box_invalidate_filter (list);
  select_terminal (list, terminal_mdi_container_get_active_screen (sidebar->container));
}

static void
update_project_expander (GtkWidget *button,
                         GtkImage *icon,
                         gboolean collapsed)
{
  gtk_image_set_from_icon_name (icon, collapsed ? "pan-end-symbolic" : "pan-down-symbolic",
                               GTK_ICON_SIZE_MENU);
  atk_object_set_description (gtk_widget_get_accessible (button),
                              collapsed ? _("Expand project") : _("Collapse project"));
}

static void
update_collapsed_projects (GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_free char **uris = g_settings_get_strv (sidebar->settings, TERMINAL_SETTING_COLLAPSED_PROJECTS_KEY);
  GtkListBoxRow *row;

  g_hash_table_remove_all (sidebar->collapsed);
  for (char **uri = uris; *uri != NULL; uri++)
    g_hash_table_add (sidebar->collapsed, *uri);

  for (int i = 0; (row = gtk_list_box_get_row_at_index (list, i)) != NULL; i++) {
    GtkWidget *toggle = g_object_get_data (G_OBJECT (row), PROJECT_TOGGLE_KEY);
    if (toggle != NULL) {
      const char *id = project_id (row);
      GtkImage *icon = g_object_get_data (G_OBJECT (toggle), "project-expander");
      update_project_expander (toggle, icon, g_hash_table_contains (sidebar->collapsed, id));
    }
  }

  gtk_list_box_invalidate_filter (list);
  mark_row (&sidebar->search_target, NULL, "project-search-target");
  select_terminal (list, terminal_mdi_container_get_active_screen (sidebar->container));
}

static void
set_project_collapsed (ProjectSidebar *sidebar,
                       const char *uri,
                       gboolean collapsed)
{
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key;

  if (g_hash_table_contains (sidebar->collapsed, uri) == collapsed)
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_STRING_ARRAY);
  g_hash_table_iter_init (&iter, sidebar->collapsed);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (!g_str_equal (key, uri))
      g_variant_builder_add (&builder, "s", key);
  if (collapsed)
    g_variant_builder_add (&builder, "s", uri);

  if (!g_settings_set_value (sidebar->settings, TERMINAL_SETTING_COLLAPSED_PROJECTS_KEY,
                             g_variant_builder_end (&builder)))
    show_error (sidebar->window, _("Could not save the project’s collapsed state"), NULL);
}

static void
toggle_project (GtkButton *button,
                GtkListBoxRow *row)
{
  ProjectSidebar *sidebar = get_sidebar (get_row_list (row));
  const char *id = project_id (row);

  set_project_collapsed (sidebar, id, !g_hash_table_contains (sidebar->collapsed, id));
}

static void
open_project_tab (GtkListBoxRow *row,
                  char **command,
                  gboolean split)
{
  ProjectSidebar *sidebar = get_sidebar (get_row_list (row));
  const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);
  gboolean remote = is_remote_project (uri);
  gs_unref_object GFile *directory = uri != NULL && !remote ? g_file_new_for_uri (uri) : NULL;
  gs_free char *path = remote ? dup_remote_project_path (uri) :
                      directory != NULL ? g_file_get_path (directory) : NULL;

  if (directory != NULL && (path == NULL || !g_file_test (path, G_FILE_TEST_IS_DIR))) {
    gs_free char *name = g_file_get_parse_name (directory);
    show_error (sidebar->window, _("Working directory is unavailable"), name);
    return;
  }

  TerminalApp *app = terminal_app_get ();
  gs_unref_object GSettings *profile =
    terminal_settings_list_ref_default_child (terminal_app_get_profiles_list (app));
  TerminalScreen *active = terminal_mdi_container_get_active_screen (sidebar->container);
  char **environment = active != NULL ? terminal_screen_get_initial_environment (active) : NULL;

  if (remote) {
    gs_unref_variant GVariant *projects =
      g_settings_get_value (sidebar->settings, TERMINAL_SETTING_REMOTE_PROJECTS_KEY);
    gs_free_error GError *error = NULL;
    const char *ssh_command, *credential, *login_command;

    if (path == NULL ||
        !g_variant_lookup (projects, uri, "(&s&s&s)", &ssh_command, &credential, &login_command) ||
        *ssh_command == '\0') {
      show_error (sidebar->window, _("Could not open the remote project"),
                  _("The saved SSH project details are unavailable."));
      return;
    }

    set_project_collapsed (sidebar, uri, FALSE);
    if (!terminal_app_open_ssh_project (app, sidebar->window, profile, environment,
                                        ssh_command, *credential != '\0' ? credential : NULL,
                                        path, *login_command != '\0' ? login_command : NULL, split, &error))
      show_error (sidebar->window, _("Could not open the remote project"),
                  error != NULL ? error->message : NULL);
    return;
  }

  TerminalNewTerminalFlags flags = uri != NULL ? TERMINAL_NEW_TERMINAL_PROJECT :
                                                TERMINAL_NEW_TERMINAL_DEFAULT;

  if (split)
    flags |= TERMINAL_NEW_TERMINAL_SPLIT;

  set_project_collapsed (sidebar, project_id (row), FALSE);
  terminal_app_new_terminal (app, sidebar->window, profile, NULL, command, NULL, path,
                             environment, 1.0, flags);
}

static void
open_project_terminal (GtkMenuItem *item,
                       GtkListBoxRow *row)
{
  open_project_tab (row, NULL, FALSE);
}

static void
open_project_split_terminal (GtkMenuItem *item,
                             GtkListBoxRow *row)
{
  open_project_tab (row, NULL, TRUE);
}

static void
open_project_neovim (GtkMenuItem *item,
                     GtkListBoxRow *row)
{
  char *command[] = {
    (char *) "nvim", (char *) "+botright split", (char *) "+terminal",
    (char *) "+startinsert", (char *) ".", NULL
  };

  open_project_tab (row, command, FALSE);
}

static void
change_project_directory (GtkMenuItem *item,
                          GtkListBoxRow *row)
{
  choose_directory (get_row_list (row), g_object_get_data (G_OBJECT (row), DIRECTORY_KEY));
}

static void
remove_project (GtkMenuItem *item,
                GtkListBoxRow *row)
{
  update_project_directory (get_row_list (row), g_object_get_data (G_OBJECT (row), DIRECTORY_KEY),
                            NULL, NULL);
}

static gboolean
save_project_directories (ProjectSidebar *sidebar,
                          char **projects)
{
  if (g_settings_set_strv (sidebar->settings, TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY,
                           (const char * const *) projects))
    return TRUE;
  show_error (sidebar->window, _("Could not save the project list"), NULL);
  return FALSE;
}

static gboolean
save_remote_project_directories (ProjectSidebar *sidebar,
                                 char **projects,
                                 const char *old_uri,
                                 const char *uri,
                                 const SshProject *ssh)
{
  gs_free_error GError *error = NULL;
  gs_free char *copy = ssh != NULL ? terminal_ssh_dup_password (ssh->credential, &error) : NULL;
  if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
    show_error (sidebar->window, _("Could not save the remote project"), error->message);
    return FALSE;
  }

  gs_unref_object GSettings *settings = g_settings_new (TERMINAL_SETTING_SCHEMA);
  gs_unref_variant GVariant *remote_projects =
    g_settings_get_value (settings, TERMINAL_SETTING_REMOTE_PROJECTS_KEY);
  GVariantBuilder builder;
  GVariantIter iter;
  const char *key, *command, *credential, *login_command;
  gs_unref_hashtable GHashTable *removed_credentials = g_hash_table_new (g_str_hash, g_str_equal);
  gs_unref_hashtable GHashTable *retained_credentials = g_hash_table_new (g_str_hash, g_str_equal);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{s(sss)}"));
  g_variant_iter_init (&iter, remote_projects);
  while (g_variant_iter_next (&iter, "{&s(&s&s&s)}", &key, &command, &credential, &login_command)) {
    if (g_strcmp0 (key, old_uri) == 0 || (ssh != NULL && g_str_equal (key, uri))) {
      if (*credential != '\0')
        g_hash_table_add (removed_credentials, (gpointer) credential);
    } else {
      g_variant_builder_add (&builder, "{s(sss)}", key, command, credential, login_command);
      if (*credential != '\0')
        g_hash_table_add (retained_credentials, (gpointer) credential);
    }
  }
  if (ssh != NULL)
    g_variant_builder_add (&builder, "{s(sss)}", uri, ssh->command, copy != NULL ? copy : "",
                           ssh->login_command != NULL ? ssh->login_command : "");

  g_settings_delay (settings);
  gboolean saved_order = g_settings_set_strv (settings, TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY,
                                              (const char * const *) projects);
  gboolean saved_ssh = g_settings_set_value (settings, TERMINAL_SETTING_REMOTE_PROJECTS_KEY,
                                            g_variant_builder_end (&builder));
  if (!saved_order || !saved_ssh) {
    g_settings_revert (settings);
    if (copy != NULL)
      terminal_ssh_forget_password (copy);
    show_error (sidebar->window, _("Could not save the remote project"), NULL);
    return FALSE;
  }

  g_settings_apply (settings);
  if (g_hash_table_size (removed_credentials) > 0) {
    GHashTableIter credentials;
    gpointer removed;
    g_settings_sync ();
    g_hash_table_iter_init (&credentials, removed_credentials);
    while (g_hash_table_iter_next (&credentials, &removed, NULL))
      if (!g_hash_table_contains (retained_credentials, removed))
        terminal_ssh_forget_password (removed);
  }
  return TRUE;
}

static void
move_project (GtkMenuItem *item,
              GtkListBoxRow *row)
{
  ProjectSidebar *sidebar = get_sidebar (get_row_list (row));
  gs_strfreev char **projects = g_settings_get_strv (sidebar->settings,
                                                    TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY);
  const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);
  int direction = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "move-project-direction"));

  for (int i = 0; projects[i] != NULL; i++)
    if (g_str_equal (projects[i], uri)) {
      int next = i + direction;
      if (next >= 0 && projects[next] != NULL) {
        char *moved = projects[i];
        projects[i] = projects[next];
        projects[next] = moved;
        save_project_directories (sidebar, projects);
      }
      return;
    }
}

static GtkWidget *
new_project_menu (GtkListBoxRow *row,
                  gboolean remote,
                  gboolean can_move_up,
                  gboolean can_move_down)
{
  const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);
  GtkWidget *menu = gtk_menu_new ();
  GtkWidget *terminal = gtk_menu_item_new_with_label (_("New terminal in this project"));
  GtkWidget *split = gtk_menu_item_new_with_label (_("New split terminal tab in this project"));

  g_signal_connect_object (terminal, "activate", G_CALLBACK (open_project_terminal), row, 0);
  g_signal_connect_object (split, "activate", G_CALLBACK (open_project_split_terminal), row, 0);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), terminal);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), split);

  if (uri == NULL)
    return new_options_button (menu, _("Project options"));

  GtkWidget *neovim = gtk_menu_item_new_with_mnemonic (_("Open in _Neovim"));
  GtkWidget *change = gtk_menu_item_new_with_mnemonic (_("_Change Directory…"));
  GtkWidget *up = gtk_menu_item_new_with_mnemonic (_("Move Project _Up"));
  GtkWidget *down = gtk_menu_item_new_with_mnemonic (_("Move Project _Down"));
  GtkWidget *remove = gtk_menu_item_new_with_mnemonic (_("_Remove Project"));

  gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
  gtk_widget_set_sensitive (neovim, !remote);
  gtk_widget_set_sensitive (change, !remote);
  gtk_widget_set_sensitive (up, can_move_up);
  gtk_widget_set_sensitive (down, can_move_down);
  g_object_set_data (G_OBJECT (up), "move-project-direction", GINT_TO_POINTER (-1));
  g_object_set_data (G_OBJECT (down), "move-project-direction", GINT_TO_POINTER (1));
  g_signal_connect_object (up, "activate", G_CALLBACK (move_project), row, 0);
  g_signal_connect_object (down, "activate", G_CALLBACK (move_project), row, 0);
  g_signal_connect_object (neovim, "activate", G_CALLBACK (open_project_neovim), row, 0);
  g_signal_connect_object (change, "activate", G_CALLBACK (change_project_directory), row, 0);
  g_signal_connect_object (remove, "activate", G_CALLBACK (remove_project), row, 0);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), neovim);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), change);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), up);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), down);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), remove);
  return new_options_button (menu, _("Project options"));
}

static GtkWidget *
new_group_row (GtkListBox *list,
               const char *uri,
               GVariant *remote_projects,
               gboolean can_move_up,
               gboolean can_move_down)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  const char *title;
  gs_free char *name = NULL;
  gs_free char *display_path = NULL;
  gs_free char *detail = NULL;
  gboolean remote = is_remote_project (uri);
  PangoAttrList *attrs = pango_attr_list_new ();

#if GTK_CHECK_VERSION (3, 14, 0)
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
#endif
  gtk_widget_set_can_focus (row, FALSE);
  gtk_widget_set_margin_end (box, 4);
  gtk_widget_set_margin_top (box, 6);
  gtk_container_add (GTK_CONTAINER (row), box);

  if (uri != NULL) {
    gs_unref_object GFile *directory = remote ? NULL : g_file_new_for_uri (uri);
    gs_free char *path = remote ? dup_remote_project_path (uri) : g_file_get_path (directory);
    display_path = remote
      ? (path != NULL ? g_filename_display_name (path) : g_strdup (_("Remote Project")))
      : g_file_get_parse_name (directory);
    name = path != NULL ? g_filename_display_basename (path) : NULL;
    title = name != NULL ? name : display_path;
    if (remote) {
      const char *command, *credential, *login_command;
      if (g_variant_lookup (remote_projects, uri, "(&s&s&s)", &command, &credential, &login_command))
        detail = g_strconcat (display_path, "\n", command, NULL);
    }
    g_object_set_data_full (G_OBJECT (row), DIRECTORY_KEY, g_strdup (uri), g_free);
  }
  else
    title = _("Default Project");

  GtkWidget *toggle = gtk_button_new ();
  GtkWidget *heading = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *icon = gtk_image_new ();

  const char *heading_text = display_path != NULL ? display_path : title;
  GtkWidget *label = new_label (heading_text);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_START);
  gtk_button_set_relief (GTK_BUTTON (toggle), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text (toggle, detail != NULL ? detail : display_path);
  atk_object_set_name (gtk_widget_get_accessible (toggle), heading_text);
  g_object_set_data_full (G_OBJECT (row), PROJECT_NAME_KEY,
                          g_utf8_casefold (remote ? display_path : title, -1), g_free);
  g_object_set_data (G_OBJECT (row), PROJECT_TOGGLE_KEY, toggle);
  g_object_set_data (G_OBJECT (toggle), "project-expander", icon);
  update_project_expander (toggle, GTK_IMAGE (icon),
                           g_hash_table_contains (get_sidebar (list)->collapsed,
                                                  project_id (GTK_LIST_BOX_ROW (row))));
  g_signal_connect (toggle, "clicked", G_CALLBACK (toggle_project), row);
  gtk_box_pack_end (GTK_BOX (heading), icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (heading), label, TRUE, TRUE, 0);
  gtk_container_add (GTK_CONTAINER (toggle), heading);
  gtk_box_pack_start (GTK_BOX (box), toggle, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (box),
                    new_project_menu (GTK_LIST_BOX_ROW (row), remote, can_move_up, can_move_down),
                    FALSE, FALSE, 0);

  pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);
  return row;
}

static void
sync_title (TerminalScreen *screen,
            GParamSpec *pspec,
            GtkLabel *label)
{
  const char *title = terminal_screen_get_title (screen);

  if (title == NULL || *title == '\0')
    title = _("Terminal");
  gtk_label_set_text (label, title);
  gtk_widget_set_tooltip_text (GTK_WIDGET (label), title);
}

static void
rename_tab_response (GtkDialog *dialog,
                     int response,
                     TerminalScreen *screen)
{
  if (response == GTK_RESPONSE_ACCEPT) {
    GtkEntry *entry = g_object_get_data (G_OBJECT (dialog), "title-entry");
    gs_free char *title = g_strdup (gtk_entry_get_text (entry));

    terminal_screen_set_custom_title (screen, g_strstrip (title));
  }
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
rename_tab (TerminalWindow *window,
            TerminalScreen *screen)
{
  if (screen == NULL)
    return;

  GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Rename Tab"), GTK_WINDOW (window),
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                   _("_Rename"), GTK_RESPONSE_ACCEPT,
                                                   NULL);
  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *entry = gtk_entry_new ();
  GtkWidget *label = gtk_label_new_with_mnemonic (_("_Tab name:"));
  GtkWidget *hint = new_label (_("Leave blank to use the terminal title."));
  const char *title = terminal_screen_get_title (screen);

  gtk_container_set_border_width (GTK_CONTAINER (content), 12);
  gtk_box_set_spacing (GTK_BOX (content), 6);
  align_label_start (label);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
  gtk_label_set_ellipsize (GTK_LABEL (hint), PANGO_ELLIPSIZE_NONE);
  if (title != NULL)
    gtk_entry_set_text (GTK_ENTRY (entry), title);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), hint, FALSE, FALSE, 0);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  g_object_set_data (G_OBJECT (dialog), "title-entry", entry);
  g_signal_connect_object (dialog, "response", G_CALLBACK (rename_tab_response), screen, 0);
  g_signal_connect_object (screen, "destroy", G_CALLBACK (gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
  gtk_widget_show_all (content);
  gtk_widget_grab_focus (entry);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
rename_tab_menu_activate (GtkMenuItem *item,
                          GtkListBoxRow *row)
{
  rename_tab (get_sidebar (get_row_list (row))->window,
              g_object_get_data (G_OBJECT (row), SCREEN_KEY));
}

static void
close_terminal (GtkMenuItem *item,
                GtkListBoxRow *row)
{
  TerminalScreen *screen = g_object_get_data (G_OBJECT (row), SCREEN_KEY);

  g_signal_emit_by_name (get_sidebar (get_row_list (row))->container, "screen-close-request", screen);
}

static GtkWidget *
new_terminal_row (TerminalScreen *screen,
                  GtkListBoxRow *project)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = new_label (NULL);
  GtkWidget *menu = gtk_menu_new ();
  GtkWidget *rename = gtk_menu_item_new_with_label (_("Rename Tab…"));
  GtkWidget *close = gtk_menu_item_new_with_label (_("Close Tab"));

  gtk_widget_set_margin_start (box, PROJECT_TERMINAL_INDENT);
  gtk_widget_set_margin_end (box, 4);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "project-terminal-title");
  gtk_container_add (GTK_CONTAINER (row), box);
  gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);

  sync_title (screen, NULL, GTK_LABEL (label));
  g_signal_connect_object (screen, "notify::title", G_CALLBACK (sync_title), label, 0);
  g_object_set_data (G_OBJECT (row), SCREEN_KEY, screen);
  g_object_set_data_full (G_OBJECT (row), PROJECT_ROW_KEY, g_object_ref (project), g_object_unref);
  g_signal_connect_object (rename, "activate", G_CALLBACK (rename_tab_menu_activate), row, 0);
  g_signal_connect_object (close, "activate", G_CALLBACK (close_terminal), row, 0);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), rename);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), close);
  gtk_box_pack_end (GTK_BOX (box), new_options_button (menu, _("Tab options")), FALSE, FALSE, 0);
  return row;
}

static GtkListBoxRow *
find_terminal_row (GtkListBox *list,
                   TerminalScreen *screen)
{
  GtkListBoxRow *row;

  screen = terminal_notebook_get_tab_screen (screen);
  if (screen != NULL)
    for (int i = 0; (row = gtk_list_box_get_row_at_index (list, i)) != NULL; i++)
      if (g_object_get_data (G_OBJECT (row), SCREEN_KEY) == screen)
        return row;
  return NULL;
}

static GtkListBoxRow *
get_project_tab_at_index (GtkListBox *list,
                          int index,
                          GtkListBoxRow *project)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (list, index);

  if (row == NULL || row == project || get_project_row (row) != project)
    return NULL;
  return row;
}

static void
select_terminal (GtkListBox *list,
                 TerminalScreen *screen)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  GtkListBoxRow *row = find_terminal_row (list, screen);
  GtkListBoxRow *project = row != NULL ? get_project_row (row) : NULL;
  gboolean can_switch = FALSE;

  if (project != NULL) {
    int index = gtk_list_box_row_get_index (row);
    can_switch = get_project_tab_at_index (list, index - 1, project) != NULL ||
                 get_project_tab_at_index (list, index + 1, project) != NULL;
  }

  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (sidebar->window), "project-tab-switch");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), can_switch);

  mark_row (&sidebar->active_project, project, "active-project");

  gtk_list_box_select_row (list, row != NULL && gtk_widget_get_child_visible (GTK_WIDGET (row))
                                ? row : NULL);
}

static void
rebuild_rows (GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_strfreev char **projects = g_settings_get_strv (sidebar->settings,
                                                    TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY);
  gs_unref_variant GVariant *remote_projects =
    g_settings_get_value (sidebar->settings, TERMINAL_SETTING_REMOTE_PROJECTS_KEY);
  gs_unref_hashtable GHashTable *local_rows = g_hash_table_new_full (g_file_hash,
                                                                  (GEqualFunc) g_file_equal,
                                                                  g_object_unref, NULL);
  gs_unref_hashtable GHashTable *remote_rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gs_free_list GList *screens = terminal_mdi_container_list_tab_screens (sidebar->container);
  GList *l;
  char **project;
  GtkWidget *default_row = new_group_row (list, NULL, NULL, FALSE, FALSE);
  GtkListBoxRow *default_project = GTK_LIST_BOX_ROW (default_row);

  sidebar->active_project = NULL;
  sidebar->search_target = NULL;
  gtk_container_foreach (GTK_CONTAINER (list), (GtkCallback) gtk_widget_destroy, NULL);
  gtk_list_box_insert (list, default_row, -1);
  for (project = projects; *project != NULL; project++) {
    GtkWidget *row = new_group_row (list, *project, remote_projects, project != projects, project[1] != NULL);
    gtk_list_box_insert (list, row, -1);
    if (is_remote_project (*project)) {
      gs_free char *key = saved_remote_project_key (remote_projects, *project);
      /* Preserve existing aliases as rows, but match the first in saved order. */
      if (key != NULL && !g_hash_table_contains (remote_rows, key))
        g_hash_table_insert (remote_rows, g_steal_pointer (&key), row);
    } else {
      g_hash_table_insert (local_rows, g_file_new_for_uri (*project), row);
    }
  }

  for (l = screens; l != NULL; l = l->next) {
    TerminalScreen *screen = l->data;
    GtkListBoxRow *group = NULL;
    if (*projects != NULL) {
      const char *path;
      const char *ssh_launch = terminal_screen_get_ssh_launch (screen, &path);
      if (ssh_launch != NULL) {
        group = find_remote_project (remote_rows, ssh_launch, path);
      } else if (!terminal_screen_has_remote_session (screen) &&
                 (path = terminal_screen_get_initial_directory (screen)) != NULL) {
        gs_unref_object GFile *directory = g_file_new_for_path (path);
        while (directory != NULL) {
          group = g_hash_table_lookup (local_rows, directory);
          if (group != NULL)
            break;
          GFile *parent = g_file_get_parent (directory);
          g_object_unref (directory);
          directory = parent;
        }
      }
    }

    if (group == NULL)
      group = default_project;
    GtkListBoxRow *tail = g_object_get_data (G_OBJECT (group), PROJECT_TAIL_KEY);
    GtkWidget *row = new_terminal_row (screen, group);
    gtk_list_box_insert (list, row, gtk_list_box_row_get_index (tail != NULL ? tail : group) + 1);
    g_object_set_data (G_OBJECT (group), PROJECT_TAIL_KEY, row);
  }

  gtk_widget_show_all (GTK_WIDGET (list));
  select_terminal (list, terminal_mdi_container_get_active_screen (sidebar->container));
}

static gboolean
project_settings_changed (GSettings *settings,
                          const GQuark *keys,
                          int n_keys,
                          GtkListBox *list)
{
  GQuark order = g_quark_from_static_string (TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY);
  GQuark remote = g_quark_from_static_string (TERMINAL_SETTING_REMOTE_PROJECTS_KEY);

  if (keys == NULL)
    rebuild_rows (list);
  else
    for (int i = 0; i < n_keys; i++)
      if (keys[i] == order || keys[i] == remote) {
        rebuild_rows (list);
        break;
      }

  return FALSE;
}

static void
screen_added (TerminalMdiContainer *container,
              TerminalScreen *screen,
              GtkListBox *list)
{
  if (screen != terminal_notebook_get_tab_screen (screen))
    return;

  g_signal_connect_object (screen, "initial-directory-changed",
                           G_CALLBACK (rebuild_rows), list, G_CONNECT_SWAPPED);
  rebuild_rows (list);
}

static void
screen_removed (TerminalMdiContainer *container,
                TerminalScreen *screen,
                GtkListBox *list)
{
  if (screen != terminal_notebook_get_tab_screen (screen))
    return;

  g_signal_handlers_disconnect_by_func (screen, G_CALLBACK (rebuild_rows), list);
  rebuild_rows (list);
}

static void
screen_switched (TerminalMdiContainer *container,
                 TerminalScreen *previous,
                 TerminalScreen *screen,
                 GtkListBox *list)
{
  select_terminal (list, screen);
}

static void
activate_terminal (GtkListBox *list,
                   GtkListBoxRow *row,
                   gpointer user_data)
{
  TerminalScreen *screen = g_object_get_data (G_OBJECT (row), SCREEN_KEY);

  if (screen != NULL) {
    terminal_mdi_container_set_active_screen (get_sidebar (list)->container, screen);
    gtk_widget_grab_focus (GTK_WIDGET (screen));
  }
}

static void
action_set_title (GSimpleAction *action,
                  GVariant *parameter,
                  gpointer user_data)
{
  ProjectSidebar *sidebar = get_sidebar (user_data);

  rename_tab (sidebar->window,
              terminal_notebook_get_tab_screen (terminal_mdi_container_get_active_screen (sidebar->container)));
}

static GtkListBoxRow *
search_result_terminal (GtkListBox *list,
                        GtkListBoxRow *row)
{
  if (!gtk_widget_get_child_visible (GTK_WIDGET (row)))
    return NULL;
  if (g_object_get_data (G_OBJECT (row), SCREEN_KEY) != NULL)
    return row;
  if (get_project_row (row) == row) {
    GtkListBoxRow *first = get_project_tab_at_index (list, gtk_list_box_row_get_index (row) + 1, row);

    /* A collapsed heading acts as the result for its first terminal. */
    if (first != NULL && !gtk_widget_get_child_visible (GTK_WIDGET (first)))
      return first;
  }
  return NULL;
}

static GtkListBoxRow *
find_search_result (GtkListBox *list,
                    GtkListBoxRow *current,
                    int step)
{
  GtkListBoxRow *row, *last = NULL;

  if (current == NULL && step < 0) {
    for (int i = 0; (row = gtk_list_box_get_row_at_index (list, i)) != NULL; i++)
      if (search_result_terminal (list, row) != NULL)
        last = row;
    return last;
  }

  int i = current != NULL ? gtk_list_box_row_get_index (current) + step : 0;
  for (; i >= 0 && (row = gtk_list_box_get_row_at_index (list, i)) != NULL; i += step)
    if (search_result_terminal (list, row) != NULL)
      return row;
  return current;
}

static gboolean
search_key_pressed (GtkWidget *entry,
                    GdkEventKey *event,
                    GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  int step;

  if ((event->state & gtk_accelerator_get_default_mod_mask ()) != 0)
    return FALSE;

  switch (event->keyval) {
    case GDK_KEY_Escape: {
      TerminalScreen *screen = terminal_mdi_container_get_active_screen (sidebar->container);
      gtk_entry_set_text (GTK_ENTRY (entry), "");
      if (screen != NULL)
        gtk_widget_grab_focus (GTK_WIDGET (screen));
      return TRUE;
    }
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      step = -1;
      break;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      step = 1;
      break;
    default:
      return FALSE;
  }

  GtkListBoxRow *row = find_search_result (list, sidebar->search_target, step);
  mark_row (&sidebar->search_target, row, "project-search-target");
  if (row != NULL) {
    int x, y;
    if (gtk_widget_translate_coordinates (GTK_WIDGET (row), GTK_WIDGET (list), 0, 0, &x, &y))
      gtk_adjustment_clamp_page (gtk_scrolled_window_get_vadjustment (sidebar->scroll), y,
                                 y + gtk_widget_get_allocated_height (GTK_WIDGET (row)));
  }
  return TRUE;
}

static void
search_activate (GtkEntry *entry,
                 GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  GtkListBoxRow *row = sidebar->search_target != NULL ? sidebar->search_target
                                                     : find_search_result (list, NULL, 1);

  if (row != NULL) {
    GtkListBoxRow *terminal = search_result_terminal (list, row);
    if (terminal != NULL)
      activate_terminal (list, terminal, NULL);
  }
}

static gboolean
search_focus_out (GtkWidget *entry,
                  GdkEventFocus *event,
                  GtkListBox *list)
{
  mark_row (&get_sidebar (list)->search_target, NULL, "project-search-target");
  return FALSE;
}

static void
action_find_project (GSimpleAction *action,
                     GVariant *parameter,
                     gpointer user_data)
{
  GtkWidget *search = get_sidebar (user_data)->search;

  if (gtk_widget_get_mapped (search)) {
    gtk_widget_grab_focus (search);
    gtk_editable_select_region (GTK_EDITABLE (search), 0, -1);
  }
}

static void
action_switch_project_tab (GSimpleAction *action,
                           GVariant *parameter,
                           gpointer user_data)
{
  GtkListBox *list = user_data;
  ProjectSidebar *sidebar = get_sidebar (list);
  GtkListBoxRow *row = find_terminal_row (list, terminal_mdi_container_get_active_screen (sidebar->container));
  GtkListBoxRow *project = row != NULL ? get_project_row (row) : NULL;

  if (project == NULL)
    return;

  int index = gtk_list_box_row_get_index (row);
  int step = g_variant_get_boolean (parameter) ? 1 : -1;
  GtkListBoxRow *target = get_project_tab_at_index (list, index + step, project);

  if (target == NULL) {
    /* Wrap to the opposite end of the same project's contiguous rows. */
    GtkListBoxRow *candidate;
    for (int i = index - step;
         (candidate = get_project_tab_at_index (list, i, project)) != NULL;
         i -= step)
      target = candidate;
  }

  if (target != NULL)
    activate_terminal (list, target, NULL);
}

static ProjectUpdate
update_project_directory (GtkListBox *list,
                          const char *old_uri,
                          const char *uri,
                          const SshProject *ssh)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_strfreev char **projects = g_settings_get_strv (sidebar->settings,
                                                    TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY);
  gs_free char *old_project = NULL;
  int index = -1;
  guint n;

  if (old_uri != NULL && g_strcmp0 (old_uri, uri) == 0)
    return PROJECT_UPDATE_OK;
  gs_unref_variant GVariant *remote_projects = ssh != NULL ?
    g_settings_get_value (sidebar->settings, TERMINAL_SETTING_REMOTE_PROJECTS_KEY) : NULL;

  for (n = 0; projects[n] != NULL; n++) {
    if (g_strcmp0 (projects[n], old_uri) == 0)
      index = n;
    if (g_strcmp0 (projects[n], uri) == 0)
      return PROJECT_UPDATE_DUPLICATE;
    if (remote_projects != NULL && g_strcmp0 (projects[n], old_uri) != 0) {
      gs_free char *key = saved_remote_project_key (remote_projects, projects[n]);
      if (key != NULL && g_str_equal (key, uri))
        return PROJECT_UPDATE_DUPLICATE;
    }
  }

  if (old_uri == NULL) {
    projects = g_renew (char *, projects, n + 2);
    projects[n] = g_strdup (uri);
    projects[n + 1] = NULL;
  } else if (index < 0) {
    return PROJECT_UPDATE_OK;
  } else {
    old_project = projects[index];
    if (uri != NULL)
      projects[index] = g_strdup (uri);
    else
      for (guint i = index; i < n; i++)
        projects[i] = projects[i + 1];
  }

  gboolean saved = ssh != NULL || is_remote_project (old_project)
    ? save_remote_project_directories (sidebar, projects, old_project, uri, ssh)
    : save_project_directories (sidebar, projects);
  if (saved && old_project != NULL)
    set_project_collapsed (sidebar, old_project, FALSE);
  return saved ? PROJECT_UPDATE_OK : PROJECT_UPDATE_FAILED;
}

static void
choose_directory_response (GtkDialog *dialog,
                           int response,
                           GtkListBox *list)
{
  if (response == GTK_RESPONSE_ACCEPT) {
    gs_unref_object GFile *directory = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));

    if (directory != NULL) {
      gs_free char *uri = g_file_get_uri (directory);
      const char *old_uri = g_object_get_data (G_OBJECT (dialog), DIRECTORY_KEY);
      ProjectSidebar *sidebar = get_sidebar (list);
      ProjectUpdate result = update_project_directory (list, old_uri, uri, NULL);
      if (result == PROJECT_UPDATE_DUPLICATE) {
        show_error (sidebar->window, _("This directory is already a project"), NULL);
      } else if (result == PROJECT_UPDATE_OK && old_uri == NULL) {
        gs_free char *path = g_file_get_path (directory);
        if (path != NULL)
          show_bookmark_status (sidebar, path);
      }
    }
  }
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
choose_directory (GtkListBox *list,
                  const char *old_uri)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  GtkWidget *dialog = gtk_file_chooser_dialog_new (old_uri != NULL ? _("Change Project Directory")
                                                                   : _("Add Working Directory"),
                                                 GTK_WINDOW (sidebar->window),
                                                 GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                 old_uri != NULL ? _("_Change") : _("_Add"), GTK_RESPONSE_ACCEPT,
                                                 NULL);

  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
  gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (dialog), TRUE);
  if (old_uri != NULL) {
    gtk_file_chooser_set_current_folder_uri (GTK_FILE_CHOOSER (dialog), old_uri);
    g_object_set_data_full (G_OBJECT (dialog), DIRECTORY_KEY, g_strdup (old_uri), g_free);
  }
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  g_signal_connect_object (dialog, "response", G_CALLBACK (choose_directory_response), list, 0);
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
add_directory (GtkMenuItem *item,
               GtkListBox *list)
{
  gtk_widget_hide (get_sidebar (list)->bookmark_notice);
  choose_directory (list, NULL);
}

static ProjectUpdate
add_remote_project (GtkListBox *list,
                    const SshProject *ssh,
                    const char *path)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_free char *uri = remote_project_uri (ssh->command, path);
  gs_free_error GError *error = NULL;
  gs_strfreev char **argv =
    terminal_ssh_build_argv (ssh->command, NULL, path, ssh->login_command, TRUE, &error);

  if (argv == NULL || uri == NULL) {
    show_error (sidebar->window, _("Could not save the remote project"),
                error != NULL ? error->message : NULL);
    return PROJECT_UPDATE_FAILED;
  }

  return update_project_directory (list, NULL, uri, ssh);
}

static void
choose_remote_directory_response (GtkDialog *dialog,
                                  int response,
                                  GtkListBox *list)
{
  if (response == GTK_RESPONSE_ACCEPT) {
    GtkEntry *entry = g_object_get_data (G_OBJECT (dialog), "remote-directory-entry");
    GtkLabel *status = g_object_get_data (G_OBJECT (dialog), "remote-directory-status");
    GVariant *connection = g_object_get_data (G_OBJECT (dialog), "remote-project-ssh");
    const char *path = gtk_entry_get_text (entry);
    SshProject ssh;

    if (path[0] != '/') {
      gtk_label_set_text (status, _("Enter an absolute directory starting with a slash (/)."));
      gtk_widget_show (GTK_WIDGET (status));
      return;
    }

    g_variant_get (connection, "(&s&s&s)", &ssh.command, &ssh.credential, &ssh.login_command);
    if (*ssh.credential != '\0') {
      GWeakRef *ref = g_object_get_data (G_OBJECT (dialog), "remote-project-screen");
      gs_unref_object TerminalScreen *screen = g_weak_ref_get (ref);
      const char *active_credential = NULL;
      if (screen != NULL)
        terminal_screen_get_active_ssh (screen, &active_credential, NULL);
      if (g_strcmp0 (active_credential, ssh.credential) != 0)
        ssh.credential = NULL;
    } else {
      ssh.credential = NULL;
    }
    if (*ssh.login_command == '\0')
      ssh.login_command = NULL;
    ProjectUpdate result = add_remote_project (list, &ssh, path);
    if (result == PROJECT_UPDATE_DUPLICATE) {
      gtk_label_set_text (status, _("This directory is already a project. Enter a different directory."));
      gtk_widget_show (GTK_WIDGET (status));
      gtk_widget_grab_focus (GTK_WIDGET (entry));
      gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
      return;
    }
    if (result == PROJECT_UPDATE_FAILED)
      return;
    show_bookmark_status (get_sidebar (list), path);
  }
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
free_screen_ref (gpointer data)
{
  g_weak_ref_clear (data);
  g_free (data);
}

static void
choose_remote_directory (GtkListBox *list,
                         TerminalScreen *screen,
                         const SshProject *ssh)
{
  ProjectSidebar *sidebar = get_sidebar (list);

  if (ssh->command == NULL || *ssh->command == '\0') {
    show_error (sidebar->window, _("Could not save the remote project"),
                _("The SSH connection details are unavailable."));
    return;
  }

  GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Add Remote Directory"),
                                                   GTK_WINDOW (sidebar->window),
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                   _("_Add"), GTK_RESPONSE_ACCEPT, NULL);
  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gs_free char *text = g_strdup_printf (_("SSH command: %s"), ssh->command);
  GtkWidget *description = gtk_label_new (text);
  GtkWidget *label = gtk_label_new_with_mnemonic (_("_Absolute directory:"));
  GtkWidget *entry = gtk_entry_new ();
  GtkWidget *status = gtk_label_new (NULL);

  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  align_label_start (description);
  gtk_label_set_line_wrap (GTK_LABEL (description), TRUE);
  gtk_label_set_line_wrap_mode (GTK_LABEL (description), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars (GTK_LABEL (description), 52);
  align_label_start (label);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  align_label_start (status);
  gtk_label_set_line_wrap (GTK_LABEL (status), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (status), 52);
  gtk_widget_set_no_show_all (status, TRUE);
  gtk_box_pack_start (GTK_BOX (box), description, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), status, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), box, TRUE, TRUE, 0);
  g_object_set_data (G_OBJECT (dialog), "remote-directory-entry", entry);
  g_object_set_data (G_OBJECT (dialog), "remote-directory-status", status);
  g_object_set_data_full (G_OBJECT (dialog), "remote-project-ssh",
                          g_variant_ref_sink (g_variant_new ("(sss)", ssh->command,
                                                            ssh->credential != NULL ? ssh->credential : "",
                                                            ssh->login_command != NULL ? ssh->login_command : "")),
                          (GDestroyNotify) g_variant_unref);
  if (ssh->credential != NULL) {
    GWeakRef *ref = g_new (GWeakRef, 1);
    g_weak_ref_init (ref, screen);
    g_object_set_data_full (G_OBJECT (dialog), "remote-project-screen", ref, free_screen_ref);
  }
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  g_signal_connect_object (dialog, "response", G_CALLBACK (choose_remote_directory_response), list, 0);
  gtk_widget_show_all (box);
  gtk_widget_grab_focus (entry);
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
add_current_directory (GtkMenuItem *item,
                       GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  TerminalScreen *screen = terminal_mdi_container_get_active_screen (sidebar->container);

  gtk_widget_hide (sidebar->bookmark_notice);

  if (screen != NULL) {
    SshProject ssh = { 0 };
    ssh.command = terminal_screen_get_active_ssh (screen, &ssh.credential, &ssh.login_command);
    gs_free char *foreground_ssh = NULL;
    if (ssh.command == NULL)
      ssh.command = foreground_ssh = terminal_screen_dup_foreground_ssh (screen);
    if (ssh.command != NULL || terminal_screen_has_remote_session (screen)) {
      choose_remote_directory (list, screen, &ssh);
      return;
    }
  }

  gs_free char *path = screen != NULL ? terminal_screen_get_current_dir (screen) : NULL;

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_DIR)) {
    show_error (sidebar->window, _("Could not determine the current local directory"), NULL);
    return;
  }

  gs_unref_object GFile *directory = g_file_new_for_path (path);
  gs_free char *uri = g_file_get_uri (directory);
  ProjectUpdate result = update_project_directory (list, NULL, uri, NULL);
  if (result == PROJECT_UPDATE_OK)
    show_bookmark_status (sidebar, path);
  else if (result == PROJECT_UPDATE_DUPLICATE) {
    gs_free char *display_path = g_filename_display_name (path);
    show_project_message (sidebar->window, GTK_MESSAGE_INFO,
                          _("This directory is already a project"), display_path);
  }
}

static gboolean
show_sidebar_on_left (GBinding *binding,
                     const GValue *position,
                     GValue *visible,
                     gpointer user_data)
{
  g_value_set_boolean (visible, g_value_get_enum (position) == GTK_POS_LEFT);
  return TRUE;
}

GtkWidget *
terminal_project_sidebar_new (TerminalWindow *window,
                              TerminalMdiContainer *container)
{
  static const GActionEntry actions[] = {
    { "set-title", action_set_title, NULL, NULL, NULL },
    { "find-project", action_find_project, NULL, NULL, NULL },
    { "project-tab-switch", action_switch_project_tab, "b", NULL, NULL }
  };
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *search = gtk_search_entry_new ();
  GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
  GtkWidget *list = gtk_list_box_new ();
  GtkWidget *empty = gtk_label_new (_("No matching projects"));
  GtkWidget *bookmark_notice = gtk_info_bar_new ();
  GtkWidget *bookmark_status = gtk_label_new (NULL);
  GtkWidget *menu = gtk_menu_new ();
  GtkWidget *add = gtk_menu_item_new_with_mnemonic (_("_Add Working Directory…"));
  GtkWidget *pin = gtk_menu_item_new_with_label (_("Add Current Directory"));
  ProjectSidebar *sidebar = g_new0 (ProjectSidebar, 1);

  sidebar->window = window;
  sidebar->container = container;
  sidebar->settings = terminal_app_get_global_settings (terminal_app_get ());
  sidebar->search = search;
  sidebar->bookmark_notice = bookmark_notice;
  sidebar->bookmark_status = bookmark_status;
  sidebar->scroll = GTK_SCROLLED_WINDOW (scroll);
  sidebar->collapsed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full (G_OBJECT (list), "terminal-project-sidebar", sidebar, free_sidebar);
  g_action_map_add_action_entries (G_ACTION_MAP (window), actions, G_N_ELEMENTS (actions), list);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (list), filter_sidebar_row, sidebar, NULL);

  gtk_widget_set_size_request (column, 200, -1);
  gtk_entry_set_placeholder_text (GTK_ENTRY (search), _("Search projects"));
  atk_object_set_name (gtk_widget_get_accessible (search), _("Search projects"));
  gtk_widget_set_margin_start (header, 6);
  gtk_widget_set_margin_end (header, 6);
  gtk_widget_set_margin_top (header, 6);
  gtk_widget_set_margin_bottom (header, 6);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), add);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), pin);
  gtk_box_pack_start (GTK_BOX (header), search, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (header), new_options_button (menu, _("Sidebar options")), FALSE, FALSE, 0);
  gtk_label_set_line_wrap (GTK_LABEL (empty), TRUE);
  gtk_widget_set_margin_start (empty, 12);
  gtk_widget_set_margin_end (empty, 12);
  gtk_widget_set_margin_top (empty, 18);
  gtk_style_context_add_class (gtk_widget_get_style_context (empty), "dim-label");
  gtk_list_box_set_placeholder (GTK_LIST_BOX (list), empty);
  gtk_widget_show (empty);
  gtk_style_context_add_class (gtk_widget_get_style_context (list), "sidebar");
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add (GTK_CONTAINER (scroll), list);
  align_label_start (bookmark_status);
  gtk_label_set_line_wrap (GTK_LABEL (bookmark_status), TRUE);
  gtk_label_set_line_wrap_mode (GTK_LABEL (bookmark_status), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars (GTK_LABEL (bookmark_status), 22);
  gtk_container_add (GTK_CONTAINER (gtk_info_bar_get_content_area (GTK_INFO_BAR (bookmark_notice))),
                     bookmark_status);
  gtk_widget_show_all (bookmark_notice);
  gtk_widget_hide (bookmark_notice);
  gtk_widget_set_no_show_all (bookmark_notice, TRUE);
  gtk_box_pack_start (GTK_BOX (column), header, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (column), scroll, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (column), bookmark_notice, FALSE, FALSE, 0);
  gtk_widget_show_all (column);
  gtk_widget_set_no_show_all (column, TRUE);
  g_object_bind_property_full (container, "tab-pos", column, "visible", G_BINDING_SYNC_CREATE,
                               show_sidebar_on_left, NULL, NULL, NULL);
  g_object_bind_property (column, "visible",
                          g_action_map_lookup_action (G_ACTION_MAP (window), "find-project"),
                          "enabled", G_BINDING_SYNC_CREATE);

  g_signal_connect_object (sidebar->settings, "change-event",
                           G_CALLBACK (project_settings_changed), list, 0);
  g_signal_connect_object (sidebar->settings, "changed::" TERMINAL_SETTING_COLLAPSED_PROJECTS_KEY,
                           G_CALLBACK (update_collapsed_projects), list, G_CONNECT_SWAPPED);
  g_signal_connect_object (container, "screen-added", G_CALLBACK (screen_added), list, G_CONNECT_AFTER);
  g_signal_connect_object (container, "screen-removed", G_CALLBACK (screen_removed), list, G_CONNECT_AFTER);
  g_signal_connect_object (container, "screen-switched", G_CALLBACK (screen_switched), list, G_CONNECT_AFTER);
  g_signal_connect_object (container, "screens-reordered", G_CALLBACK (rebuild_rows), list,
                           G_CONNECT_SWAPPED | G_CONNECT_AFTER);
  g_signal_connect (list, "row-activated", G_CALLBACK (activate_terminal), NULL);
  g_signal_connect_object (search, "changed", G_CALLBACK (search_projects), list, 0);
  g_signal_connect_object (search, "key-press-event", G_CALLBACK (search_key_pressed), list, 0);
  g_signal_connect_object (search, "focus-out-event", G_CALLBACK (search_focus_out), list, 0);
  g_signal_connect_object (search, "activate", G_CALLBACK (search_activate), list, 0);
  g_signal_connect_object (add, "activate", G_CALLBACK (add_directory), list, 0);
  g_signal_connect_object (pin, "activate", G_CALLBACK (add_current_directory), list, 0);
  update_collapsed_projects (GTK_LIST_BOX (list));
  rebuild_rows (GTK_LIST_BOX (list));
  return column;
}
