/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "config.h"

#include <glib/gi18n.h>
#include <string.h>

#include "terminal-app.h"
#include "terminal-libgsystem.h"
#include "terminal-project-sidebar.h"
#include "terminal-schemas.h"

#define PROJECT_TERMINAL_INDENT 76
#define SCREEN_KEY "terminal-project-screen"
#define DIRECTORY_KEY "terminal-project-directory"
#define PROJECT_ROW_KEY "terminal-project-row"
#define PROJECT_NAME_KEY "terminal-project-search-name"
#define PROJECT_TOGGLE_KEY "terminal-project-toggle"
#define PROJECT_COUNT_KEY "terminal-project-count"

typedef struct {
  TerminalWindow *window;
  TerminalMdiContainer *container;
  GSettings *settings;
  GHashTable *collapsed;
  GtkListBoxRow *active_project;
  GtkListBoxRow *search_target;
  GtkWidget *search;
  GtkScrolledWindow *scroll;
  char *search_query;
} ProjectSidebar;

static void select_terminal (GtkListBox *list, TerminalScreen *screen);
static void choose_directory (GtkListBox *list, const char *old_uri);
static void update_project_directory (GtkListBox *list, const char *old_uri, GFile *directory);

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
  if (g_object_get_data (G_OBJECT (row), DIRECTORY_KEY) != NULL)
    return row;
  return g_object_get_data (G_OBJECT (row), PROJECT_ROW_KEY);
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
filter_sidebar_row (GtkListBoxRow *row,
                    gpointer user_data)
{
  ProjectSidebar *sidebar = user_data;
  GtkListBoxRow *project = get_project_row (row);
  const char *uri;

  if (project == NULL)
    return sidebar->search_query == NULL;
  if (sidebar->search_query != NULL &&
      strstr (g_object_get_data (G_OBJECT (project), PROJECT_NAME_KEY), sidebar->search_query) == NULL)
    return FALSE;
  if (row == project)
    return TRUE;

  uri = g_object_get_data (G_OBJECT (project), DIRECTORY_KEY);
  return !g_hash_table_contains (sidebar->collapsed, uri);
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
      const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);
      GtkImage *icon = g_object_get_data (G_OBJECT (toggle), "project-expander");
      update_project_expander (toggle, icon, g_hash_table_contains (sidebar->collapsed, uri));
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
  const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);

  set_project_collapsed (sidebar, uri, !g_hash_table_contains (sidebar->collapsed, uri));
}

static void
open_project_terminal (GtkButton *button,
                       GtkListBoxRow *row)
{
  ProjectSidebar *sidebar = get_sidebar (get_row_list (row));
  const char *uri = g_object_get_data (G_OBJECT (row), DIRECTORY_KEY);
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

  set_project_collapsed (sidebar, uri, FALSE);
  terminal_app_new_terminal (app, sidebar->window, profile, NULL, NULL, NULL, path,
                             active ? terminal_screen_get_initial_environment (active) : NULL,
                             1.0);
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
  update_project_directory (get_row_list (row), g_object_get_data (G_OBJECT (row), DIRECTORY_KEY), NULL);
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
                  gboolean can_move_up,
                  gboolean can_move_down)
{
  GtkWidget *button = gtk_menu_button_new ();
  GtkWidget *menu = gtk_menu_new ();
  GtkWidget *change = gtk_menu_item_new_with_mnemonic (_("_Change Directory…"));
  GtkWidget *up = gtk_menu_item_new_with_mnemonic (_("Move Project _Up"));
  GtkWidget *down = gtk_menu_item_new_with_mnemonic (_("Move Project _Down"));
  GtkWidget *remove = gtk_menu_item_new_with_mnemonic (_("_Remove Project"));

  gtk_button_set_image (GTK_BUTTON (button),
                        gtk_image_new_from_icon_name ("view-more-symbolic", GTK_ICON_SIZE_MENU));
  gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text (button, _("Project options"));
  atk_object_set_name (gtk_widget_get_accessible (button), _("Project options"));
  gtk_widget_set_sensitive (up, can_move_up);
  gtk_widget_set_sensitive (down, can_move_down);
  g_object_set_data (G_OBJECT (up), "move-project-direction", GINT_TO_POINTER (-1));
  g_object_set_data (G_OBJECT (down), "move-project-direction", GINT_TO_POINTER (1));
  g_signal_connect_object (up, "activate", G_CALLBACK (move_project), row, 0);
  g_signal_connect_object (down, "activate", G_CALLBACK (move_project), row, 0);
  g_signal_connect_object (change, "activate", G_CALLBACK (change_project_directory), row, 0);
  g_signal_connect_object (remove, "activate", G_CALLBACK (remove_project), row, 0);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), change);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), up);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), down);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), remove);
  gtk_widget_show_all (menu);
  gtk_menu_button_set_popup (GTK_MENU_BUTTON (button), menu);
  return button;
}

static GtkWidget *
new_group_row (GtkListBox *list,
               const char *uri,
               gboolean can_move_up,
               gboolean can_move_down)
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
    GtkWidget *folder = gtk_image_new_from_icon_name ("folder-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *count = gtk_label_new (NULL);
    GtkWidget *menu = new_project_menu (GTK_LIST_BOX_ROW (row), can_move_up, can_move_down);

    label = new_label (title);
    gtk_button_set_relief (GTK_BUTTON (toggle), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (toggle, detail);
    atk_object_set_name (gtk_widget_get_accessible (toggle), title);
    g_object_set_data_full (G_OBJECT (row), DIRECTORY_KEY, g_strdup (uri), g_free);
    g_object_set_data_full (G_OBJECT (row), PROJECT_NAME_KEY, g_utf8_casefold (title, -1), g_free);
    g_object_set_data (G_OBJECT (row), PROJECT_TOGGLE_KEY, toggle);
    g_object_set_data (G_OBJECT (row), PROJECT_COUNT_KEY, count);
    g_object_set_data (G_OBJECT (toggle), "project-expander", icon);
    update_project_expander (toggle, GTK_IMAGE (icon),
                             g_hash_table_contains (get_sidebar (list)->collapsed, uri));
    g_signal_connect (toggle, "clicked", G_CALLBACK (toggle_project), row);
    gtk_box_pack_start (GTK_BOX (heading), icon, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (heading), folder, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (heading), label, TRUE, TRUE, 0);
    gtk_container_add (GTK_CONTAINER (toggle), heading);
    gtk_box_pack_start (GTK_BOX (box), toggle, TRUE, TRUE, 0);
    g_signal_connect (button, "clicked", G_CALLBACK (open_project_terminal), row);
    gtk_style_context_add_class (gtk_widget_get_style_context (count), "dim-label");
    gtk_widget_set_margin_start (count, 4);
    gtk_widget_set_tooltip_text (count, _("Open terminals"));
    gtk_box_pack_start (GTK_BOX (box), count, FALSE, FALSE, 0);
    gtk_box_pack_end (GTK_BOX (box), menu, FALSE, FALSE, 0);
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
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
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
rename_tab_clicked (GtkButton *button,
                    GtkListBoxRow *row)
{
  rename_tab (get_sidebar (get_row_list (row))->window,
              g_object_get_data (G_OBJECT (row), SCREEN_KEY));
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
                  TerminalScreen *screen,
                  GtkListBoxRow *project)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = new_label (NULL);
  GtkWidget *rename = new_icon_button ("document-edit-symbolic", _("Rename Tab"));
  GtkWidget *button = new_icon_button ("window-close-symbolic", _("Close tab"));

  gtk_widget_set_margin_start (box, project != NULL ? PROJECT_TERMINAL_INDENT : 20);
  gtk_widget_set_margin_end (box, 4);
  gtk_container_add (GTK_CONTAINER (row), box);
  gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (box), button, FALSE, FALSE, 0);
  gtk_box_pack_end (GTK_BOX (box), rename, FALSE, FALSE, 0);

  sync_title (screen, NULL, GTK_LABEL (label));
  g_signal_connect_object (screen, "notify::title", G_CALLBACK (sync_title), label, 0);
  g_object_set_data (G_OBJECT (row), SCREEN_KEY, screen);
  if (project != NULL)
    g_object_set_data_full (G_OBJECT (row), PROJECT_ROW_KEY, g_object_ref (project), g_object_unref);
  g_signal_connect (rename, "clicked", G_CALLBACK (rename_tab_clicked), row);
  g_object_set_data (G_OBJECT (button), SCREEN_KEY, screen);
  g_signal_connect (button, "clicked", G_CALLBACK (close_terminal), list);
  return row;
}

static GtkListBoxRow *
find_terminal_row (GtkListBox *list,
                   TerminalScreen *screen)
{
  GtkListBoxRow *row;

  if (screen != NULL)
    for (int i = 0; (row = gtk_list_box_get_row_at_index (list, i)) != NULL; i++)
      if (g_object_get_data (G_OBJECT (row), SCREEN_KEY) == screen)
        return row;
  return NULL;
}

static void
select_terminal (GtkListBox *list,
                 TerminalScreen *screen)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  GtkListBoxRow *row = find_terminal_row (list, screen);
  GtkListBoxRow *project = row != NULL ? get_project_row (row) : NULL;

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
  gs_unref_hashtable GHashTable *tails = g_hash_table_new_full (g_file_hash,
                                                             (GEqualFunc) g_file_equal,
                                                             g_object_unref, NULL);
  gs_free_list GList *screens = terminal_mdi_container_list_screens (sidebar->container);
  GList *l;
  char **project;
  gboolean has_other = FALSE;

  sidebar->active_project = NULL;
  sidebar->search_target = NULL;
  gtk_container_foreach (GTK_CONTAINER (list), (GtkCallback) gtk_widget_destroy, NULL);
  for (project = projects; *project != NULL; project++) {
    GtkWidget *row = new_group_row (list, *project, project != projects, project[1] != NULL);
    gtk_list_box_insert (list, row, -1);
    g_hash_table_insert (tails, g_file_new_for_uri (*project), row);
  }

  for (l = screens; l != NULL; l = l->next) {
    TerminalScreen *screen = l->data;
    const char *path = terminal_screen_get_initial_directory (screen);
    GtkListBoxRow *tail = NULL;
    gs_unref_object GFile *directory = NULL;

    if (path != NULL && *projects != NULL) {
      directory = g_file_new_for_path (path);
      while (directory != NULL) {
        GFile *parent;

        tail = g_hash_table_lookup (tails, directory);
        if (tail != NULL)
          break;
        parent = g_file_get_parent (directory);
        g_object_unref (directory);
        directory = parent;
      }
    }

    GtkListBoxRow *group = tail != NULL ? get_project_row (tail) : NULL;
    GtkWidget *row = new_terminal_row (list, screen, group);
    if (tail != NULL) {
      gtk_list_box_insert (list, row, gtk_list_box_row_get_index (tail) + 1);
      g_hash_table_insert (tails, g_steal_pointer (&directory), row);
    } else {
      if (!has_other) {
        gtk_list_box_insert (list, new_group_row (list, NULL, FALSE, FALSE), -1);
        has_other = TRUE;
      }
      gtk_list_box_insert (list, row, -1);
    }
  }

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init (&iter, tails);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    GtkListBoxRow *tail = value;
    GtkListBoxRow *group = get_project_row (tail);
    GtkLabel *count = g_object_get_data (G_OBJECT (group), PROJECT_COUNT_KEY);
    int n_tabs = gtk_list_box_row_get_index (tail) - gtk_list_box_row_get_index (group);
    char text[32];

    g_snprintf (text, sizeof text, "%d", n_tabs);
    gtk_label_set_text (count, text);
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
action_set_title (GSimpleAction *action,
                  GVariant *parameter,
                  gpointer user_data)
{
  ProjectSidebar *sidebar = get_sidebar (user_data);

  rename_tab (sidebar->window, terminal_mdi_container_get_active_screen (sidebar->container));
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

static void
update_project_directory (GtkListBox *list,
                          const char *old_uri,
                          GFile *directory)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  gs_strfreev char **projects = g_settings_get_strv (sidebar->settings,
                                                    TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY);
  gs_free char *uri = directory != NULL ? g_file_get_uri (directory) : NULL;
  gs_free char *old_project = NULL;
  int index = -1;
  guint n;

  if (old_uri != NULL && g_strcmp0 (old_uri, uri) == 0)
    return;

  for (n = 0; projects[n] != NULL; n++) {
    if (g_strcmp0 (projects[n], old_uri) == 0)
      index = n;
    if (g_strcmp0 (projects[n], uri) == 0) {
      if (old_uri != NULL)
        show_error (sidebar->window, _("This directory is already a project"), NULL);
      return;
    }
  }

  if (old_uri == NULL) {
    projects = g_renew (char *, projects, n + 2);
    projects[n] = g_steal_pointer (&uri);
    projects[n + 1] = NULL;
  } else if (index < 0) {
    return;
  } else {
    old_project = projects[index];
    if (uri != NULL)
      projects[index] = g_steal_pointer (&uri);
    else
      for (guint i = index; i < n; i++)
        projects[i] = projects[i + 1];
  }

  if (save_project_directories (sidebar, projects) && old_project != NULL)
    set_project_collapsed (sidebar, old_project, FALSE);
}

static void
choose_directory_response (GtkDialog *dialog,
                           int response,
                           GtkListBox *list)
{
  if (response == GTK_RESPONSE_ACCEPT) {
    gs_unref_object GFile *directory = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));

    if (directory != NULL)
      update_project_directory (list, g_object_get_data (G_OBJECT (dialog), DIRECTORY_KEY), directory);
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
add_directory (GtkButton *button,
               GtkListBox *list)
{
  choose_directory (list, NULL);
}

static void
add_current_directory (GtkButton *button,
                       GtkListBox *list)
{
  ProjectSidebar *sidebar = get_sidebar (list);
  TerminalScreen *screen = terminal_mdi_container_get_active_screen (sidebar->container);
  gs_free char *path = screen != NULL ? terminal_screen_get_current_dir (screen) : NULL;

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_DIR)) {
    show_error (sidebar->window, _("Could not determine the current local directory"), NULL);
    return;
  }

  gs_unref_object GFile *directory = g_file_new_for_path (path);
  update_project_directory (list, NULL, directory);
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
  GtkWidget *search = gtk_search_entry_new ();
  GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
  GtkWidget *list = gtk_list_box_new ();
  GtkWidget *empty = gtk_label_new (_("No matching projects"));
  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *add = gtk_button_new_with_mnemonic (_("_Add Working Directory…"));
  GtkWidget *pin = new_icon_button ("bookmark-new-symbolic", _("Add Current Directory"));
  ProjectSidebar *sidebar = g_new0 (ProjectSidebar, 1);

  sidebar->window = window;
  sidebar->container = container;
  sidebar->settings = terminal_app_get_global_settings (terminal_app_get ());
  sidebar->search = search;
  sidebar->scroll = GTK_SCROLLED_WINDOW (scroll);
  sidebar->collapsed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full (G_OBJECT (list), "terminal-project-sidebar", sidebar, free_sidebar);
  g_action_map_add_action_entries (G_ACTION_MAP (window), actions, G_N_ELEMENTS (actions), list);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (list), filter_sidebar_row, sidebar, NULL);

  gtk_widget_set_size_request (column, 200, -1);
  gtk_entry_set_placeholder_text (GTK_ENTRY (search), _("Search projects"));
  atk_object_set_name (gtk_widget_get_accessible (search), _("Search projects"));
  gtk_widget_set_margin_start (search, 6);
  gtk_widget_set_margin_end (search, 6);
  gtk_widget_set_margin_top (search, 6);
  gtk_widget_set_margin_bottom (search, 6);
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
  gtk_widget_set_margin_start (footer, 6);
  gtk_widget_set_margin_end (footer, 6);
  gtk_widget_set_margin_top (footer, 6);
  gtk_widget_set_margin_bottom (footer, 6);
  gtk_box_pack_start (GTK_BOX (footer), add, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (footer), pin, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (column), search, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (column), scroll, TRUE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (column), footer, FALSE, FALSE, 0);
  gtk_widget_show_all (column);
  gtk_widget_set_no_show_all (column, TRUE);
  g_object_bind_property_full (container, "tab-pos", column, "visible", G_BINDING_SYNC_CREATE,
                               show_sidebar_on_left, NULL, NULL, NULL);

  g_signal_connect_object (sidebar->settings, "changed::" TERMINAL_SETTING_PROJECT_DIRECTORIES_KEY,
                           G_CALLBACK (rebuild_rows), list, G_CONNECT_SWAPPED);
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
  g_signal_connect_object (add, "clicked", G_CALLBACK (add_directory), list, 0);
  g_signal_connect_object (pin, "clicked", G_CALLBACK (add_current_directory), list, 0);
  update_collapsed_projects (GTK_LIST_BOX (list));
  rebuild_rows (GTK_LIST_BOX (list));
  return column;
}
