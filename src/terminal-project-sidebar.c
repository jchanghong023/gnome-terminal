/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "config.h"

#include <glib/gi18n.h>

#include "terminal-app.h"
#include "terminal-libgsystem.h"
#include "terminal-project-sidebar.h"
#include "terminal-schemas.h"

#define SIDEBAR_WIDTH 220
#define SCREEN_KEY "terminal-project-screen"
#define DIRECTORY_KEY "terminal-project-directory"

typedef struct {
  TerminalWindow *window;
  TerminalMdiContainer *container;
  GSettings *settings;
  GHashTable *collapsed;
} ProjectSidebar;

static void select_terminal (GtkListBox *list, TerminalScreen *screen);

static void
free_sidebar (gpointer data)
{
  ProjectSidebar *sidebar = data;

  g_hash_table_unref (sidebar->collapsed);
  g_free (sidebar);
}

static ProjectSidebar *
get_sidebar (GtkListBox *list)
{
  return g_object_get_data (G_OBJECT (list), "terminal-project-sidebar");
}

static void
show_error (TerminalWindow *window,
            const char *message,
            const char *detail)
{
  GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                             GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                             GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                             "%s", message);
  if (detail != NULL)
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", detail);
  g_signal_connect_swapped (dialog, "response", G_CALLBACK (gtk_widget_destroy), dialog);
  gtk_widget_show (dialog);
}

static GtkWidget *
new_label (const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  return label;
}

static GtkWidget *
new_icon_button (const char *icon,
                 const char *name)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon, GTK_ICON_SIZE_MENU);

  gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text (button, name);
  atk_object_set_name (gtk_widget_get_accessible (button), name);
  return button;
}

static gboolean
filter_terminal (GtkListBoxRow *row,
                 gpointer user_data)
{
  ProjectSidebar *sidebar = user_data;
  const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);

  return uri == NULL || !g_hash_table_contains (sidebar->collapsed, uri);
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
toggle_project (GtkButton *button,
                GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  const char *uri = g_object_get_data (G_OBJECT (button), DIRECTORY_KEY);
  GtkImage *icon = g_object_get_data (G_OBJECT (button), "project-expander");
  gboolean collapsed = !g_hash_table_remove (sidebar->collapsed, uri);

  if (collapsed)
    g_hash_table_add (sidebar->collapsed, g_strdup (uri));
  update_project_expander (GTK_WIDGET (button), icon, collapsed);
  gtk_list_box_invalidate_filter (list);
  select_terminal (list, terminal_mdi_container_get_active_screen (sidebar->container));
}

static void
open_project_terminal (GtkButton *button,
                       GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  const char *uri = g_object_get_data (G_OBJECT (button), DIRECTORY_KEY);
  gs_unref_object GFile *directory = g_file_new_for_uri (uri);
  gs_free char *path = g_file_get_path (directory);

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_DIR)) {
    gs_free char *name = g_file_get_parse_name (directory);
    show_error (sidebar->window, _("Working directory is unavailable"), name);
    return;
  }

  TerminalApp *app = terminal_app_get ();
  gs_unref_object GSettings *profile =
    terminal_settings_list_ref_default_child (terminal_app_get_profiles_list (app));
  TerminalScreen *active = terminal_mdi_container_get_active_screen (sidebar->container);

  g_hash_table_remove (sidebar->collapsed, uri);
  terminal_app_new_terminal (app, sidebar->window, profile, NULL, NULL, NULL, path,
                             active ? terminal_screen_get_initial_environment (active) : NULL,
                             1.0);
}

static GtkWidget *
new_group_row (GtkListBox *list,
               const char *uri)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label;
  PangoAttrList *attrs = pango_attr_list_new ();

  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_widget_set_can_focus (row, FALSE);
  gtk_widget_set_margin_start (box, 10);
  gtk_widget_set_margin_end (box, 4);
  gtk_widget_set_margin_top (box, 6);
  gtk_container_add (GTK_CONTAINER (row), box);

  if (uri != NULL) {
    gs_unref_object GFile *directory = g_file_new_for_uri (uri);
    gs_free char *path = g_file_get_path (directory);
    gs_free char *title = path ? g_filename_display_basename (path) : g_file_get_parse_name (directory);
    gs_free char *detail = g_file_get_parse_name (directory);
    GtkWidget *button = new_icon_button ("list-add-symbolic", _("New terminal in this project"));
    GtkWidget *toggle = gtk_button_new ();
    GtkWidget *heading = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *icon = gtk_image_new ();

    label = new_label (title);
    gtk_button_set_relief (GTK_BUTTON (toggle), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (toggle, detail);
    atk_object_set_name (gtk_widget_get_accessible (toggle), title);
    g_object_set_data_full (G_OBJECT (toggle), DIRECTORY_KEY, g_strdup (uri), g_free);
    g_object_set_data (G_OBJECT (toggle), "project-expander", icon);
    update_project_expander (toggle, GTK_IMAGE (icon),
                             g_hash_table_contains (get_sidebar (list)->collapsed, uri));
    g_signal_connect (toggle, "clicked", G_CALLBACK (toggle_project), list);
    gtk_box_pack_start (GTK_BOX (heading), icon, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (heading), label, TRUE, TRUE, 0);
    gtk_container_add (GTK_CONTAINER (toggle), heading);
    gtk_box_pack_start (GTK_BOX (box), toggle, TRUE, TRUE, 0);
    g_object_set_data_full (G_OBJECT (button), DIRECTORY_KEY, g_strdup (uri), g_free);
    g_signal_connect (button, "clicked", G_CALLBACK (open_project_terminal), list);
    gtk_box_pack_end (GTK_BOX (box), button, FALSE, FALSE, 0);
  } else {
    label = new_label (_("Other Terminals"));
    gtk_widget_set_margin_bottom (label, 6);
    gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);
  }

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
close_terminal (GtkButton *button,
                GtkListBox *list)
{
  TerminalScreen *screen = g_object_get_data (G_OBJECT (button), SCREEN_KEY);

  g_signal_emit_by_name (get_sidebar (list)->container, "screen-close-request", screen);
}

static GtkWidget *
new_terminal_row (GtkListBox *list,
                  TerminalScreen *screen)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = new_label (NULL);
  GtkWidget *button = new_icon_button ("window-close-symbolic", _("Close tab"));

  gtk_widget_set_margin_start (box, 20);
  gtk_widget_set_margin_end (box, 4);
  gtk_container_add (GTK_CONTAINER (row), box);
  gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (box), button, FALSE, FALSE, 0);

  sync_title (screen, NULL, GTK_LABEL (label));
  g_signal_connect_object (screen, "notify::title", G_CALLBACK (sync_title), label, 0);
  g_object_set_data (G_OBJECT (row), SCREEN_KEY, screen);
  g_object_set_data (G_OBJECT (button), SCREEN_KEY, screen);
  g_signal_connect (button, "clicked", G_CALLBACK (close_terminal), list);
  return row;
}

static void
select_terminal (GtkListBox *list,
                 TerminalScreen *screen)
{
  GtkListBoxRow *row = NULL;

  if (screen != NULL) {
    int i;
    for (i = 0; (row = gtk_list_box_get_row_at_index (list, i)) != NULL; i++) {
      if (g_object_get_data (G_OBJECT (row), SCREEN_KEY) == screen)
        break;
    }
  }
  gtk_list_box_select_row (list, row != NULL && gtk_widget_get_child_visible (GTK_WIDGET (row))
                                ? row : NULL);
}

static void
rebuild_rows (GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_strfreev char **projects = g_settings_get_strv (sidebar->settings,
                                                    TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY);
  gs_unref_hashtable GHashTable *tails = g_hash_table_new (g_str_hash, g_str_equal);
  gs_free_list GList *screens = terminal_mdi_container_list_screens (sidebar->container);
  GList *l;
  char **project;
  gboolean has_other = FALSE;

  gtk_container_foreach (GTK_CONTAINER (list), (GtkCallback) gtk_widget_destroy, NULL);
  for (project = projects; *project != NULL; project++) {
    GtkWidget *row = new_group_row (list, *project);
    gtk_list_box_insert (list, row, -1);
    g_hash_table_insert (tails, *project, row);
  }

  for (l = screens; l != NULL; l = l->next) {
    TerminalScreen *screen = l->data;
    const char *path = terminal_screen_get_initial_directory (screen);
    GtkListBoxRow *tail = NULL;
    gpointer key = NULL, value = NULL;

    if (path != NULL && *projects != NULL) {
      gs_unref_object GFile *directory = g_file_new_for_path (path);
      gs_free char *uri = g_file_get_uri (directory);
      if (g_hash_table_lookup_extended (tails, uri, &key, &value))
        tail = GTK_LIST_BOX_ROW (value);
    }

    GtkWidget *row = new_terminal_row (list, screen);
    if (tail != NULL) {
      g_object_set_data_full (G_OBJECT (row), DIRECTORY_KEY, g_strdup (key), g_free);
      gtk_list_box_insert (list, row, gtk_list_box_row_get_index (tail) + 1);
      g_hash_table_insert (tails, key, row);
    } else {
      if (!has_other) {
        gtk_list_box_insert (list, new_group_row (list, NULL), -1);
        has_other = TRUE;
      }
      gtk_list_box_insert (list, row, -1);
    }
  }

  gtk_widget_show_all (GTK_WIDGET (list));
  select_terminal (list, terminal_mdi_container_get_active_screen (sidebar->container));
}

static void
screen_added (TerminalMdiContainer *container,
              TerminalScreen *screen,
              GtkListBox *list)
{
  g_signal_connect_object (screen, "initial-directory-changed",
                           G_CALLBACK (rebuild_rows), list, G_CONNECT_SWAPPED);
  rebuild_rows (list);
}

static void
screen_removed (TerminalMdiContainer *container,
                TerminalScreen *screen,
                GtkListBox *list)
{
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
choose_directory_response (GtkDialog *dialog,
                           int response,
                           GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_unref_object GFile *directory = NULL;

  if (response == GTK_RESPONSE_ACCEPT)
    directory = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
  gtk_widget_destroy (GTK_WIDGET (dialog));

  if (directory != NULL) {
    gs_free char *uri = g_file_get_uri (directory);
    gs_strfreev char **projects = g_settings_get_strv (sidebar->settings,
                                                      TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY);
    guint n = 0;

    while (projects[n] != NULL) {
      if (g_str_equal (projects[n], uri))
        return;
      n++;
    }
    projects = g_renew (char *, projects, n + 2);
    projects[n] = g_steal_pointer (&uri);
    projects[n + 1] = NULL;
    if (!g_settings_set_strv (sidebar->settings, TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY,
                             (const char * const *) projects))
      show_error (sidebar->window, _("Could not save the working directory"), NULL);
  }
}

static void
choose_directory (GtkButton *button,
                  GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  GtkWidget *dialog = gtk_file_chooser_dialog_new (_("Add Working Directory"),
                                                 GTK_WINDOW (sidebar->window),
                                                 GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                 _("_Add"), GTK_RESPONSE_ACCEPT,
                                                 NULL);

  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
  gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (dialog), TRUE);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  g_signal_connect_object (dialog, "response", G_CALLBACK (choose_directory_response), list, 0);
  gtk_window_present (GTK_WINDOW (dialog));
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
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
  GtkWidget *list = gtk_list_box_new ();
  GtkWidget *add = gtk_button_new_with_mnemonic (_("_Add Working Directory…"));
  ProjectSidebar *sidebar = g_new (ProjectSidebar, 1);

  sidebar->window = window;
  sidebar->container = container;
  sidebar->settings = terminal_app_get_global_settings (terminal_app_get ());
  sidebar->collapsed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full (G_OBJECT (list), "terminal-project-sidebar", sidebar, free_sidebar);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (list), filter_terminal, sidebar, NULL);

  gtk_widget_set_size_request (column, SIDEBAR_WIDTH, -1);
  gtk_style_context_add_class (gtk_widget_get_style_context (list), "sidebar");
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add (GTK_CONTAINER (scroll), list);
  gtk_widget_set_margin_start (add, 6);
  gtk_widget_set_margin_end (add, 6);
  gtk_widget_set_margin_top (add, 6);
  gtk_widget_set_margin_bottom (add, 6);
  gtk_box_pack_start (GTK_BOX (column), scroll, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (column), add, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (outer), column, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (outer), gtk_separator_new (GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
  gtk_widget_show_all (outer);
  gtk_widget_set_no_show_all (outer, TRUE);
  g_object_bind_property_full (container, "tab-pos", outer, "visible", G_BINDING_SYNC_CREATE,
                               show_sidebar_on_left, NULL, NULL, NULL);

  g_signal_connect_object (sidebar->settings, "changed::" TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY,
                           G_CALLBACK (rebuild_rows), list, G_CONNECT_SWAPPED);
  g_signal_connect_object (container, "screen-added", G_CALLBACK (screen_added), list, G_CONNECT_AFTER);
  g_signal_connect_object (container, "screen-removed", G_CALLBACK (screen_removed), list, G_CONNECT_AFTER);
  g_signal_connect_object (container, "screen-switched", G_CALLBACK (screen_switched), list, G_CONNECT_AFTER);
  g_signal_connect_object (container, "screens-reordered", G_CALLBACK (rebuild_rows), list,
                           G_CONNECT_SWAPPED | G_CONNECT_AFTER);
  g_signal_connect (list, "row-activated", G_CALLBACK (activate_terminal), NULL);
  g_signal_connect_object (add, "clicked", G_CALLBACK (choose_directory), list, 0);
  rebuild_rows (GTK_LIST_BOX (list));
  return outer;
}
