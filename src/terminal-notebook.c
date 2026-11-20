/*
 * Copyright © 2001 Havoc Pennington
 * Copyright © 2002 Red Hat, Inc.
 * Copyright © 2008, 2010, 2011, 2012 Christian Persch
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

#include "terminal-notebook.h"

#include <gtk/gtk.h>

#include "terminal-debug.h"
#include "terminal-app.h"
#include "terminal-intl.h"
#include "terminal-mdi-container.h"
#include "terminal-screen-container.h"
#include "terminal-tab-label.h"
#include "terminal-schemas.h"
#include "terminal-libgsystem.h"

#define TERMINAL_NOTEBOOK_GET_PRIVATE(notebook)(G_TYPE_INSTANCE_GET_PRIVATE ((notebook), TERMINAL_TYPE_NOTEBOOK, TerminalNotebookPrivate))

struct _TerminalNotebookPrivate
{
  TerminalScreen *active_screen;
  gboolean switching_page;
  GtkPolicyType policy;
};

enum
{
  PROP_0,
  PROP_ACTIVE_SCREEN,
  PROP_TAB_POLICY
};

#define ACTION_AREA_BORDER_WIDTH (2)
#define ACTION_BUTTON_SPACING (6)

/* helper functions */

static void
update_tab_visibility (TerminalNotebook *notebook,
                       int change)
{
  TerminalNotebookPrivate *priv = notebook->priv;
  GtkNotebook *gtk_notebook = GTK_NOTEBOOK (notebook);
  int new_n_pages;
  gboolean show_tabs;

  if (gtk_widget_in_destruction (GTK_WIDGET (notebook)))
    return;

  if (gtk_notebook_get_tab_pos (gtk_notebook) == GTK_POS_LEFT) {
    gtk_notebook_set_show_tabs (gtk_notebook, FALSE);
    return;
  }

  new_n_pages = gtk_notebook_get_n_pages (gtk_notebook) + change;
  /* Don't do anything if we're going to have zero pages (and thus close the window) */
  if (new_n_pages == 0)
    return;

  switch (priv->policy) {
  case GTK_POLICY_ALWAYS:
    show_tabs = TRUE;
    break;
  case GTK_POLICY_AUTOMATIC:
    show_tabs = new_n_pages > 1;
    break;
  case GTK_POLICY_NEVER:
#if GTK_CHECK_VERSION (3, 16, 0)
  case GTK_POLICY_EXTERNAL:
#endif
  default:
    show_tabs = FALSE;
    break;
  }

  gtk_notebook_set_show_tabs (gtk_notebook, show_tabs);
}

static void
tab_position_changed_cb (GtkNotebook *notebook,
                         GParamSpec *pspec,
                         gpointer user_data)
{
  update_tab_visibility (TERMINAL_NOTEBOOK (notebook), 0);
}

static void
close_button_clicked_cb (TerminalTabLabel *tab_label,
                         gpointer user_data)
{
  TerminalScreen *screen;
  TerminalNotebook *notebook;

  screen = terminal_tab_label_get_screen (tab_label);

  /* notebook is not passed as user_data because it can change during DND
   * and the close button is not notified about that, see bug 731998.
   */
  notebook = TERMINAL_NOTEBOOK (gtk_widget_get_ancestor (GTK_WIDGET (screen),
                                                         TERMINAL_TYPE_NOTEBOOK));

  if (notebook != NULL)
    g_signal_emit_by_name (notebook, "screen-close-request", screen);
}


static void
remove_reorder_bindings (GtkBindingSet    *binding_set,
                         guint             keysym)
{
  guint keypad_keysym = keysym - GDK_KEY_Left + GDK_KEY_KP_Left;
  gtk_binding_entry_skip (binding_set, keysym, GDK_MOD1_MASK);
  gtk_binding_entry_skip (binding_set, keypad_keysym, GDK_MOD1_MASK);
}

/* Single-terminal pages are screen containers; split pages are homogeneous
 * boxes holding two screen containers. The first screen identifies the tab. */
static GtkWidget *
page_for_screen (TerminalScreen *screen)
{
  GtkWidget *container = GTK_WIDGET (terminal_screen_container_get_from_screen (screen));
  GtkWidget *parent = container ? gtk_widget_get_parent (container) : NULL;

  return GTK_IS_BOX (parent) ? parent : container;
}

TerminalScreen *
terminal_notebook_get_page_screen (GtkWidget *page)
{
  if (GTK_IS_BOX (page))
    return g_object_get_data (G_OBJECT (page), "split-left");

  return TERMINAL_IS_SCREEN_CONTAINER (page) ?
         terminal_screen_container_get_screen (TERMINAL_SCREEN_CONTAINER (page)) : NULL;
}

static TerminalScreen *
split_right (GtkWidget *page)
{
  return GTK_IS_BOX (page) ? g_object_get_data (G_OBJECT (page), "split-right") : NULL;
}

TerminalScreen *
terminal_notebook_get_tab_screen (TerminalScreen *screen)
{
  return screen ? terminal_notebook_get_page_screen (page_for_screen (screen)) : NULL;
}

TerminalScreen *
terminal_notebook_get_peer (TerminalScreen *screen)
{
  GtkWidget *page = page_for_screen (screen);
  TerminalScreen *right = split_right (page);

  if (right == NULL)
    return NULL;

  return screen == right ? terminal_notebook_get_page_screen (page) : right;
}

static void
set_active_screen (TerminalNotebook *notebook,
                   TerminalScreen *screen)
{
  TerminalScreen *old_screen = notebook->priv->active_screen;

  if (screen == old_screen)
    return;

  notebook->priv->active_screen = screen;
  g_signal_emit_by_name (notebook, "screen-switched", old_screen, screen);
  g_object_notify (G_OBJECT (notebook), "active-screen");
}

void
terminal_notebook_focus_screen (TerminalNotebook *notebook,
                                TerminalScreen *screen)
{
  GtkNotebook *gtk_notebook = GTK_NOTEBOOK (notebook);

  int position = gtk_notebook_get_current_page (gtk_notebook);

  if (!notebook->priv->switching_page && position >= 0 &&
      page_for_screen (screen) == gtk_notebook_get_nth_page (gtk_notebook, position))
    set_active_screen (notebook, screen);
}

static void
set_page_screens_visible (GtkWidget *page,
                          gboolean visible)
{
  TerminalScreen *left = terminal_notebook_get_page_screen (page);
  TerminalScreen *right = split_right (page);

  if (left)
    gtk_widget_set_visible (GTK_WIDGET (left), visible);
  if (right)
    gtk_widget_set_visible (GTK_WIDGET (right), visible);
}

static void
insert_page (TerminalNotebook *notebook,
             GtkWidget *page,
             GtkWidget *tab_label,
             int position)
{
  GtkNotebook *gtk_notebook = GTK_NOTEBOOK (notebook);

  update_tab_visibility (notebook, +1);
  gtk_notebook_insert_page (gtk_notebook, page, tab_label, position);
  gtk_container_child_set (GTK_CONTAINER (notebook), page,
                           "tab-expand", TRUE,
                           "tab-fill", TRUE,
                           NULL);
  gtk_notebook_set_tab_reorderable (gtk_notebook, page, TRUE);
}

static void
add_tab (TerminalNotebook *notebook,
         TerminalScreen *left,
         TerminalScreen *right)
{
  GtkWidget *page = terminal_screen_container_new (left);
  GtkWidget *tab_label = terminal_tab_label_new (left);

  if (right != NULL) {
    GtkWidget *split = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);
    GtkWidget *right_container = terminal_screen_container_new (right);

    gtk_box_set_homogeneous (GTK_BOX (split), TRUE);
    gtk_box_pack_start (GTK_BOX (split), page, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (split), right_container, TRUE, TRUE, 0);
    gtk_widget_show (page);
    gtk_widget_show (right_container);
    g_object_set_data (G_OBJECT (split), "split-left", left);
    g_object_set_data (G_OBJECT (split), "split-right", right);
    page = split;
  }

  gtk_widget_show (page);
  g_signal_connect (tab_label, "close-button-clicked",
                    G_CALLBACK (close_button_clicked_cb), NULL);
  insert_page (notebook, page, tab_label, -1);
}

void
terminal_notebook_add_split (TerminalNotebook *notebook,
                             TerminalScreen *left,
                             TerminalScreen *right)
{
  add_tab (notebook, left, right);
}

void
terminal_notebook_move_tab (TerminalNotebook *source,
                            TerminalNotebook *dest,
                            TerminalScreen *screen,
                            int dest_position)
{
  if (source == dest)
    return;

  GtkWidget *page = page_for_screen (screen);
  GtkWidget *tab_label = gtk_notebook_get_tab_label (GTK_NOTEBOOK (source), page);

  g_object_ref (page);
  g_object_ref (tab_label);
  terminal_mdi_container_remove_screen (TERMINAL_MDI_CONTAINER (source), screen);
  insert_page (dest, page, tab_label, dest_position);
  terminal_mdi_container_set_active_screen (TERMINAL_MDI_CONTAINER (dest), screen);
  gtk_widget_grab_focus (GTK_WIDGET (screen));
  g_object_unref (tab_label);
  g_object_unref (page);
}

/* TerminalMdiContainer impl */

static void
terminal_notebook_add_screen (TerminalMdiContainer *container,
                              TerminalScreen *screen)
{
  add_tab (TERMINAL_NOTEBOOK (container), screen, NULL);
}

static void
terminal_notebook_remove_screen (TerminalMdiContainer *container,
                                 TerminalScreen *screen)
{
  TerminalNotebook *notebook = TERMINAL_NOTEBOOK (container);

  g_warn_if_fail (gtk_widget_is_ancestor (GTK_WIDGET (screen), GTK_WIDGET (notebook)));

  update_tab_visibility (notebook, -1);

  gtk_container_remove (GTK_CONTAINER (notebook), page_for_screen (screen));
}

static TerminalScreen *
terminal_notebook_get_active_screen (TerminalMdiContainer *container)
{
  return TERMINAL_NOTEBOOK (container)->priv->active_screen;
}

static void
terminal_notebook_set_active_screen (TerminalMdiContainer *container,
                                     TerminalScreen *screen)
{
  TerminalNotebook *notebook = TERMINAL_NOTEBOOK (container);
  GtkNotebook *gtk_notebook = GTK_NOTEBOOK (notebook);
  int position = gtk_notebook_page_num (gtk_notebook, page_for_screen (screen));

  if (position < 0)
    return;

  gtk_notebook_set_current_page (gtk_notebook, position);
  set_active_screen (notebook, screen);
}

static GList *
terminal_notebook_list_screen_containers (TerminalMdiContainer *container)
{
  GList *containers = NULL;
  int n_pages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (container));

  for (int i = n_pages - 1; i >= 0; i--) {
    GtkWidget *page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (container), i);
    TerminalScreen *right = split_right (page);
    if (right)
      containers = g_list_prepend (containers, terminal_screen_container_get_from_screen (right));
    containers = g_list_prepend (containers,
                                 terminal_screen_container_get_from_screen (terminal_notebook_get_page_screen (page)));
  }

  return containers;
}

static GList *
terminal_notebook_list_tab_screens (TerminalMdiContainer *container)
{
  GList *list, *l;

  list = gtk_container_get_children (GTK_CONTAINER (container));
  for (l = list; l != NULL; l = l->next)
    l->data = terminal_notebook_get_page_screen (l->data);

  return list;
}

static int
terminal_notebook_get_n_screens (TerminalMdiContainer *container)
{
  return gtk_notebook_get_n_pages (GTK_NOTEBOOK (container));
}

static int
terminal_notebook_get_active_screen_num (TerminalMdiContainer *container)
{
  return gtk_notebook_get_current_page (GTK_NOTEBOOK (container));
}

static void
terminal_notebook_set_active_screen_num (TerminalMdiContainer *container,
                                         int position)
{
  GtkNotebook *gtk_notebook = GTK_NOTEBOOK (container);

  gtk_notebook_set_current_page (gtk_notebook, position);
}

static void
terminal_notebook_reorder_screen (TerminalMdiContainer *container,
                                  TerminalScreen *screen,
                                  int new_position)
{
  GtkNotebook *notebook = GTK_NOTEBOOK (container);
  GtkWidget *child;
  int n, pos;

  g_return_if_fail (new_position == 1 || new_position == -1);

  child = page_for_screen (screen);
  n = gtk_notebook_get_n_pages (notebook);
  pos = gtk_notebook_page_num (notebook, child);

  pos += new_position;
  gtk_notebook_reorder_child (notebook, child,
                              pos < 0 ? n - 1 : pos < n ? pos : 0);
}

static void
terminal_notebook_mdi_iface_init (TerminalMdiContainerInterface *iface)
{
  iface->add_screen = terminal_notebook_add_screen;
  iface->remove_screen = terminal_notebook_remove_screen;
  iface->get_active_screen = terminal_notebook_get_active_screen;
  iface->set_active_screen = terminal_notebook_set_active_screen;
  iface->list_tab_screens = terminal_notebook_list_tab_screens;
  iface->list_screen_containers = terminal_notebook_list_screen_containers;
  iface->get_n_screens = terminal_notebook_get_n_screens;
  iface->get_active_screen_num = terminal_notebook_get_active_screen_num;
  iface->set_active_screen_num = terminal_notebook_set_active_screen_num;
  iface->reorder_screen = terminal_notebook_reorder_screen;
}

G_DEFINE_TYPE_WITH_CODE (TerminalNotebook, terminal_notebook, GTK_TYPE_NOTEBOOK,
                         G_IMPLEMENT_INTERFACE (TERMINAL_TYPE_MDI_CONTAINER, terminal_notebook_mdi_iface_init))

/* GtkNotebookClass impl */

static void
terminal_notebook_switch_page (GtkNotebook     *gtk_notebook,
                               GtkWidget       *child,
                               guint            page_num)
{
  TerminalNotebook *notebook = TERMINAL_NOTEBOOK (gtk_notebook);
  TerminalNotebookPrivate *priv = notebook->priv;
  TerminalScreen *old_screen = priv->active_screen;
  GtkWidget *old_page = old_screen ? page_for_screen (old_screen) : NULL;
  GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (notebook));
  TerminalScreen *screen = terminal_notebook_get_page_screen (child);

  priv->switching_page = TRUE;
  set_page_screens_visible (child, TRUE);
  GTK_NOTEBOOK_CLASS (terminal_notebook_parent_class)->switch_page (gtk_notebook, child, page_num);

  /* Hidden terminals should not contribute to the notebook's size request. */
  if (old_page != child)
    set_page_screens_visible (old_page, FALSE);

  if (GTK_IS_WINDOW (toplevel)) {
    GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (toplevel));
    if (TERMINAL_IS_SCREEN (focus) && page_for_screen (TERMINAL_SCREEN (focus)) == child)
      screen = TERMINAL_SCREEN (focus);
  }
  priv->switching_page = FALSE;
  set_active_screen (notebook, screen);
}

static void
terminal_notebook_page_added (GtkNotebook     *gtk_notebook,
                              GtkWidget       *child,
                              guint            page_num)
{
  TerminalNotebook *notebook = TERMINAL_NOTEBOOK (gtk_notebook);
  void (* page_added) (GtkNotebook *, GtkWidget *, guint) =
    GTK_NOTEBOOK_CLASS (terminal_notebook_parent_class)->page_added;

  if (page_added)
    page_added (gtk_notebook, child, page_num);

  update_tab_visibility (notebook, 0);
  g_signal_emit_by_name (gtk_notebook, "screen-added",
                         terminal_notebook_get_page_screen (child));
  if (split_right (child))
    g_signal_emit_by_name (gtk_notebook, "screen-added", split_right (child));
}

static void
terminal_notebook_page_removed (GtkNotebook     *gtk_notebook,
                                GtkWidget       *child,
                                guint            page_num)
{
  TerminalNotebook *notebook = TERMINAL_NOTEBOOK (gtk_notebook);
  void (* page_removed) (GtkNotebook *, GtkWidget *, guint) =
    GTK_NOTEBOOK_CLASS (terminal_notebook_parent_class)->page_removed;

  if (page_removed)
    page_removed (gtk_notebook, child, page_num);

  update_tab_visibility (notebook, 0);
  if (gtk_notebook_get_n_pages (gtk_notebook) == 0)
    set_active_screen (TERMINAL_NOTEBOOK (gtk_notebook), NULL);

  /* The last removal may destroy the window; disconnect the peer first. */
  if (split_right (child))
    g_signal_emit_by_name (gtk_notebook, "screen-removed", split_right (child));
  g_signal_emit_by_name (gtk_notebook, "screen-removed",
                         terminal_notebook_get_page_screen (child));
}

static void
terminal_notebook_page_reordered (GtkNotebook     *notebook,
                                  GtkWidget       *child,
                                  guint            page_num)
{
  void (* page_reordered) (GtkNotebook *, GtkWidget *, guint) =
    GTK_NOTEBOOK_CLASS (terminal_notebook_parent_class)->page_reordered;

  if (page_reordered)
    page_reordered (notebook, child, page_num);

  g_signal_emit_by_name (notebook, "screens-reordered");
}

static GtkNotebook *
terminal_notebook_create_window (GtkNotebook       *notebook,
                                 GtkWidget         *page,
                                 gint               x,
                                 gint               y)
{
  return GTK_NOTEBOOK_CLASS (terminal_notebook_parent_class)->create_window (notebook, page, x, y);
}

/* GtkWidgetClass impl */

static void
terminal_notebook_grab_focus (GtkWidget *widget)
{
  TerminalScreen *screen;

  screen = terminal_mdi_container_get_active_screen (TERMINAL_MDI_CONTAINER (widget));
  if (screen)
    gtk_widget_grab_focus (GTK_WIDGET (screen));
}

/* Tab scrolling was removed from GtkNotebook in gtk 3, so reimplement it here */
static gboolean
terminal_notebook_scroll_event (GtkWidget      *widget,
                                GdkEventScroll *event)
{
  GtkNotebook *notebook = GTK_NOTEBOOK (widget);
  gboolean (* scroll_event) (GtkWidget *, GdkEventScroll *) =
    GTK_WIDGET_CLASS (terminal_notebook_parent_class)->scroll_event;
  GtkWidget *child, *event_widget, *action_widget;

  if ((event->state & gtk_accelerator_get_default_mod_mask ()) != 0)
    goto chain_up;

  child = gtk_notebook_get_nth_page (notebook, gtk_notebook_get_current_page (notebook));
  if (child == NULL)
    goto chain_up;

  event_widget = gtk_get_event_widget ((GdkEvent *) event);

  /* Ignore scroll events from the content of the page */
  if (event_widget == NULL ||
      event_widget == child ||
      gtk_widget_is_ancestor (event_widget, child))
    goto chain_up;

  /* And also from the action widgets */
  action_widget = gtk_notebook_get_action_widget (notebook, GTK_PACK_START);
  if (event_widget == action_widget ||
      (action_widget != NULL && gtk_widget_is_ancestor (event_widget, action_widget)))
    goto chain_up;
  action_widget = gtk_notebook_get_action_widget (notebook, GTK_PACK_END);
  if (event_widget == action_widget ||
      (action_widget != NULL && gtk_widget_is_ancestor (event_widget, action_widget)))
    goto chain_up;

  switch (event->direction) {
    case GDK_SCROLL_RIGHT:
    case GDK_SCROLL_DOWN:
      gtk_notebook_next_page (notebook);
      return TRUE;
    case GDK_SCROLL_LEFT:
    case GDK_SCROLL_UP:
      gtk_notebook_prev_page (notebook);
      return TRUE;
    case GDK_SCROLL_SMOOTH:
      switch (gtk_notebook_get_tab_pos (notebook)) {
        case GTK_POS_LEFT:
        case GTK_POS_RIGHT:
          if (event->delta_y > 0)
            gtk_notebook_next_page (notebook);
          else if (event->delta_y < 0)
            gtk_notebook_prev_page (notebook);
          break;
        case GTK_POS_TOP:
        case GTK_POS_BOTTOM:
          if (event->delta_x > 0)
            gtk_notebook_next_page (notebook);
          else if (event->delta_x < 0)
            gtk_notebook_prev_page (notebook);
          break;
      }
      return TRUE;
  }

chain_up:
  if (scroll_event)
    return scroll_event (widget, event);

  return FALSE;
}

/* GObjectClass impl */

static void
terminal_notebook_init (TerminalNotebook *notebook)
{
  TerminalNotebookPrivate *priv;

  priv = notebook->priv = TERMINAL_NOTEBOOK_GET_PRIVATE (notebook);

  priv->active_screen = NULL;
  priv->policy = GTK_POLICY_AUTOMATIC;
}

static void
terminal_notebook_constructed (GObject *object)
{
  GSettings *settings;
  GtkWidget *widget = GTK_WIDGET (object);
  GtkNotebook *notebook = GTK_NOTEBOOK (object);

  G_OBJECT_CLASS (terminal_notebook_parent_class)->constructed (object);

  settings = terminal_app_get_global_settings (terminal_app_get ());

  g_settings_bind (settings,
                   TERMINAL_SETTING_TAB_POLICY_KEY,
                   object,
                   "tab-policy",
                   G_SETTINGS_BIND_GET | G_SETTINGS_BIND_NO_SENSITIVITY);

  g_settings_bind (settings,
                   TERMINAL_SETTING_TAB_POSITION_KEY,
                   object,
                   "tab-pos",
                   G_SETTINGS_BIND_GET | G_SETTINGS_BIND_NO_SENSITIVITY);
  g_signal_connect (notebook, "notify::tab-pos",
                    G_CALLBACK (tab_position_changed_cb), NULL);
  update_tab_visibility (TERMINAL_NOTEBOOK (notebook), 0);

  gtk_notebook_set_scrollable (notebook, TRUE);
  gtk_notebook_set_show_border (notebook, FALSE);
  gtk_notebook_set_group_name (notebook, I_("gnome-terminal-window"));

  /* Necessary for scroll events */
  gtk_widget_add_events (widget, GDK_SCROLL_MASK);
}

static void
terminal_notebook_get_property (GObject *object,
                                guint prop_id,
                                GValue *value,
                                GParamSpec *pspec)
{
  TerminalMdiContainer *mdi_container = TERMINAL_MDI_CONTAINER (object);

  switch (prop_id) {
    case PROP_ACTIVE_SCREEN:
      g_value_set_object (value, terminal_notebook_get_active_screen (mdi_container));
      break;
    case PROP_TAB_POLICY:
      g_value_set_enum (value, terminal_notebook_get_tab_policy (TERMINAL_NOTEBOOK (mdi_container)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
terminal_notebook_set_property (GObject *object,
                                guint prop_id,
                                const GValue *value,
                                GParamSpec *pspec)
{
  TerminalMdiContainer *mdi_container = TERMINAL_MDI_CONTAINER (object);

  switch (prop_id) {
    case PROP_ACTIVE_SCREEN:
      terminal_notebook_set_active_screen (mdi_container, g_value_get_object (value));
      break;
    case PROP_TAB_POLICY:
      terminal_notebook_set_tab_policy (TERMINAL_NOTEBOOK (mdi_container), g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
terminal_notebook_class_init (TerminalNotebookClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkNotebookClass *notebook_class = GTK_NOTEBOOK_CLASS (klass);

  g_type_class_add_private (gobject_class, sizeof (TerminalNotebookPrivate));

  gobject_class->constructed = terminal_notebook_constructed;
  gobject_class->get_property = terminal_notebook_get_property;
  gobject_class->set_property = terminal_notebook_set_property;

  g_object_class_override_property (gobject_class, PROP_ACTIVE_SCREEN, "active-screen");

  widget_class->grab_focus = terminal_notebook_grab_focus;
  widget_class->scroll_event = terminal_notebook_scroll_event;

  notebook_class->switch_page = terminal_notebook_switch_page;
  notebook_class->create_window = terminal_notebook_create_window;
  notebook_class->page_added = terminal_notebook_page_added;
  notebook_class->page_removed = terminal_notebook_page_removed;
  notebook_class->page_reordered = terminal_notebook_page_reordered;

  g_object_class_install_property
    (gobject_class,
     PROP_TAB_POLICY,
     g_param_spec_enum ("tab-policy", NULL, NULL,
                        GTK_TYPE_POLICY_TYPE,
                        GTK_POLICY_AUTOMATIC,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Remove unwanted and interfering keybindings */
  GtkBindingSet *binding_set = gtk_binding_set_by_class (terminal_notebook_parent_class);
  gtk_binding_entry_skip (binding_set, GDK_KEY_Page_Up, GDK_CONTROL_MASK);
  gtk_binding_entry_skip (binding_set, GDK_KEY_Page_Up, GDK_CONTROL_MASK | GDK_MOD1_MASK);
  gtk_binding_entry_skip (binding_set, GDK_KEY_Page_Down, GDK_CONTROL_MASK);
  gtk_binding_entry_skip (binding_set, GDK_KEY_Page_Down, GDK_CONTROL_MASK | GDK_MOD1_MASK);
  remove_reorder_bindings (binding_set, GDK_KEY_Up);
  remove_reorder_bindings (binding_set, GDK_KEY_Down);
  remove_reorder_bindings (binding_set, GDK_KEY_Left);
  remove_reorder_bindings (binding_set, GDK_KEY_Right);
  remove_reorder_bindings (binding_set, GDK_KEY_Home);
  remove_reorder_bindings (binding_set, GDK_KEY_Home);
  remove_reorder_bindings (binding_set, GDK_KEY_End);
  remove_reorder_bindings (binding_set, GDK_KEY_End);
}

/* public API */

/**
 * terminal_notebook_new:
 *
 * Returns: (transfer full): a new #TerminalNotebook
 */
GtkWidget *
terminal_notebook_new (void)
{
  return g_object_new (TERMINAL_TYPE_NOTEBOOK, NULL);
}

void
terminal_notebook_set_tab_policy (TerminalNotebook *notebook,
                                  GtkPolicyType policy)
{
  TerminalNotebookPrivate *priv = notebook->priv;

  if (priv->policy == policy)
    return;

  priv->policy = policy;
  update_tab_visibility (notebook, 0);

  g_object_notify (G_OBJECT (notebook), "tab-policy");
}

GtkPolicyType
terminal_notebook_get_tab_policy (TerminalNotebook *notebook)
{
  return notebook->priv->policy;
}

GtkWidget *
terminal_notebook_get_action_box (TerminalNotebook *notebook,
                                  GtkPackType pack_type)
{
  GtkNotebook *gtk_notebook;
  GtkWidget *box, *inner_box;

  g_return_val_if_fail (TERMINAL_IS_NOTEBOOK (notebook), NULL);

  gtk_notebook = GTK_NOTEBOOK (notebook);
  box = gtk_notebook_get_action_widget (gtk_notebook, pack_type);
  if (box != NULL) {
    gs_free_list GList *list;

    list = gtk_container_get_children (GTK_CONTAINER (box));
    g_assert (list->data != NULL);
    return list->data;
  }

  /* Create container for the buttons */
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_set_border_width (GTK_CONTAINER (box), ACTION_AREA_BORDER_WIDTH);

  inner_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, ACTION_BUTTON_SPACING);
  gtk_box_pack_start (GTK_BOX (box), inner_box, TRUE, FALSE, 0);
  gtk_widget_show (inner_box);

  gtk_notebook_set_action_widget (gtk_notebook, box, pack_type);
  gtk_widget_show (box);

  /* FIXME: this appears to be necessary to make the icon buttons contained
   * in the action area render the same way as buttons in the tab labels (e.g.
   * the close button). gtk+ bug?
   */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_style_context_add_region (gtk_widget_get_style_context (box),
                                GTK_STYLE_REGION_TAB,
                                pack_type == GTK_PACK_START ? GTK_REGION_FIRST : GTK_REGION_LAST);
  G_GNUC_END_IGNORE_DEPRECATIONS

  return inner_box;
}
