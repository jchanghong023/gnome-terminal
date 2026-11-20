/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <string.h>
#include <uuid.h>

#include "terminal-defines.h"
#include "terminal-libgsystem.h"
#include "terminal-ssh.h"

static char *
helper_path (void)
{
  const char *root = g_getenv (TERMINAL_PORTABLE_ENV_ROOT);
  if (root != NULL)
    return g_build_filename (root, "libexec", "gnome-terminal-ssh", NULL);

  return g_strdup (TERM_LIBEXECDIR "/gnome-terminal-ssh");
}

char **
terminal_ssh_build_argv (const char *command,
                         const char *credential,
                         GError **error)
{
  gs_strfreev char **argv = NULL;
  int argc;
  if (!g_shell_parse_argv (command, &argc, &argv, error))
    return NULL;

  if (argc < 2 || !g_str_equal (argv[0], "ssh")) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         _("Enter an OpenSSH command, for example: ssh user@host"));
    return NULL;
  }

  if (credential == NULL) {
    char **result = argv;
    argv = NULL;
    return result;
  }

  argv = g_realloc_n (argv, argc + 4, sizeof *argv);
  memmove (argv + 3, argv, (argc + 1) * sizeof *argv);
  argv[0] = helper_path ();
  argv[1] = g_strdup ("--connect");
  argv[2] = g_strdup (credential);
  char **result = argv;
  argv = NULL;
  return result;
}

static GSubprocess *
run_keyring_helper (const char *operation,
                    const char *credential,
                    gboolean store,
                    GError **error)
{
  gs_free char *helper = helper_path ();
  const char *argv[] = { helper, operation, credential, NULL };
  gs_unref_object GSubprocessLauncher *launcher =
    g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_SILENCE |
                               (store ? G_SUBPROCESS_FLAGS_STDIN_PIPE : 0));

  /* The portable Terminal server has its own bus; secrets belong to the desktop. */
  if (g_getenv (TERMINAL_PORTABLE_ENV_ROOT) != NULL) {
    gs_strfreev char **env = terminal_portable_restore_host_environment (g_get_environ ());
    g_subprocess_launcher_set_environ (launcher, env);
  }

  return g_subprocess_launcher_spawnv (launcher, argv, error);
}

void
terminal_ssh_forget_password (const char *credential)
{
  gs_unref_object GSubprocess *process = run_keyring_helper ("--forget", credential, FALSE, NULL);
}

typedef struct {
  GWeakRef screen;
  GtkEntry *command;
  GtkEntry *password;
  GtkLabel *status;
  char *pending_command;
  char *pending_credential;
  gboolean busy;
} SshDialog;

static void
ssh_dialog_free (gpointer data)
{
  SshDialog *form = data;
  g_weak_ref_clear (&form->screen);
  g_free (form->pending_command);
  g_free (form->pending_credential);
  g_free (form);
}

static void
free_password_buffer (gpointer data)
{
  for (volatile char *p = data; *p != '\0'; ++p)
    *p = '\0';
  g_free (data);
}

static void
keyring_finished (GObject *source,
                  GAsyncResult *result,
                  gpointer data)
{
  gs_unref_object GtkDialog *dialog = data;
  SshDialog *form = g_object_get_data (G_OBJECT (dialog), "ssh-form");
  GSubprocess *process = G_SUBPROCESS (source);
  gboolean success = g_subprocess_communicate_finish (process, result, NULL, NULL, NULL) &&
                     g_subprocess_get_successful (process);
  gs_unref_object TerminalScreen *screen = g_weak_ref_get (&form->screen);

  if (!gtk_widget_get_visible (GTK_WIDGET (dialog)) || screen == NULL) {
    if (success && form->pending_credential != NULL)
      terminal_ssh_forget_password (form->pending_credential);
    return;
  }

  if (success) {
    const char *old_credential = NULL;
    terminal_screen_get_startup_ssh (screen, &old_credential);
    if (form->pending_credential != NULL && old_credential != NULL &&
        !g_str_equal (form->pending_credential, old_credential))
      terminal_ssh_forget_password (old_credential);
    terminal_screen_set_startup_ssh (screen, form->pending_command, form->pending_credential);
    gtk_widget_destroy (GTK_WIDGET (dialog));
  } else {
    form->busy = FALSE;
    gtk_widget_set_sensitive (gtk_dialog_get_content_area (dialog), TRUE);
    gtk_dialog_set_response_sensitive (dialog, GTK_RESPONSE_ACCEPT, TRUE);
    gtk_dialog_set_response_sensitive (dialog, GTK_RESPONSE_REJECT, TRUE);
    gtk_dialog_set_response_sensitive (dialog, GTK_RESPONSE_CANCEL, TRUE);
    gtk_label_set_text (form->status, _("Could not update the password. Unlock your system keyring and try again."));
  }
}

static void
configure_response (GtkDialog *dialog,
                     int response,
                     SshDialog *form)
{
  if (form->busy)
    return;

  if (response != GTK_RESPONSE_ACCEPT && response != GTK_RESPONSE_REJECT) {
    gtk_widget_destroy (GTK_WIDGET (dialog));
    return;
  }

  gs_unref_object TerminalScreen *screen = g_weak_ref_get (&form->screen);
  if (screen == NULL) {
    gtk_widget_destroy (GTK_WIDGET (dialog));
    return;
  }

  const char *old_credential = NULL;
  const char *old_command = terminal_screen_get_startup_ssh (screen, &old_credential);
  gs_free char *command = response == GTK_RESPONSE_REJECT ? g_strdup (old_command) :
                            g_strstrip (g_strdup (gtk_entry_get_text (form->command)));
  const char *password = response == GTK_RESPONSE_REJECT ? "" : gtk_entry_get_text (form->password);
  gboolean store = command != NULL && *command != '\0' && *password != '\0';
  gboolean keep = response == GTK_RESPONSE_ACCEPT && !store && g_strcmp0 (command, old_command) == 0;

  if (!store && *password != '\0') {
    gtk_label_set_text (form->status, _("Enter an SSH command before saving a password."));
    return;
  }

  if (command != NULL && *command != '\0') {
    gs_strfreev char **argv = terminal_ssh_build_argv (command, NULL, NULL);
    if (argv == NULL) {
      gtk_label_set_text (form->status, _("Enter an OpenSSH command, for example: ssh user@host"));
      return;
    }
  }

  if (store && (strlen (password) > 1022 || strpbrk (password, "\r\n") != NULL)) {
    gtk_label_set_text (form->status, _("Enter a password of up to 1022 bytes without line breaks."));
    return;
  }

  if (!store && (old_credential == NULL || keep)) {
    terminal_screen_set_startup_ssh (screen, command, keep ? old_credential : NULL);
    gtk_widget_destroy (GTK_WIDGET (dialog));
    return;
  }

  char new_credential[37];
  if (store) {
    uuid_t uuid;
    uuid_generate (uuid);
    uuid_unparse (uuid, new_credential);
  }

  gs_unref_object GSubprocess *process = run_keyring_helper (store ? "--store" : "--forget",
                                                             store ? new_credential : old_credential,
                                                             store, NULL);
  if (process == NULL) {
    gtk_label_set_text (form->status, _("Could not start the system keyring helper."));
    return;
  }

  g_free (form->pending_command);
  g_free (form->pending_credential);
  form->pending_command = g_strdup (command);
  form->pending_credential = store ? g_strdup (new_credential) : NULL;
  form->busy = TRUE;
  gtk_dialog_set_response_sensitive (dialog, GTK_RESPONSE_ACCEPT, FALSE);
  gtk_dialog_set_response_sensitive (dialog, GTK_RESPONSE_REJECT, FALSE);
  gtk_dialog_set_response_sensitive (dialog, GTK_RESPONSE_CANCEL, FALSE);
  gtk_label_set_text (form->status, _("Updating the system keyring…"));
  gs_unref_bytes GBytes *input = NULL;
  if (store) {
    char *buffer = g_strdup (password);
    input = g_bytes_new_with_free_func (buffer, strlen (buffer), free_password_buffer, buffer);
  }
  g_subprocess_communicate_async (process, input, NULL, keyring_finished, g_object_ref (dialog));
  gtk_entry_set_text (form->password, "");
  gtk_widget_set_sensitive (gtk_dialog_get_content_area (dialog), FALSE);
}

static void
clear_command_status (GtkEditable *entry,
                      GtkLabel *status)
{
  gtk_label_set_text (status, "");
}

static GtkWidget *
new_form_label (const char *text,
                GtkWidget *entry)
{
  GtkWidget *label = entry != NULL ? gtk_label_new_with_mnemonic (text) : gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  if (entry != NULL)
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
  return label;
}

void
terminal_ssh_configure (GtkWindow *parent,
                        TerminalScreen *screen)
{
  if (screen == NULL)
    return;

  const char *credential = NULL;
  const char *command = terminal_screen_get_startup_ssh (screen, &credential);
  GtkWidget *dialog = gtk_dialog_new_with_buttons (_("SSH on Reopen"), parent,
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                   _("_Save"), GTK_RESPONSE_ACCEPT,
                                                   NULL);
  if (credential != NULL)
    gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Forget Password"), GTK_RESPONSE_REJECT);

  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *command_entry = gtk_entry_new ();
  GtkWidget *password_entry = gtk_entry_new ();
  GtkWidget *status = new_form_label ("", NULL);
  gtk_container_set_border_width (GTK_CONTAINER (content), 12);
  gtk_box_set_spacing (GTK_BOX (content), 6);
  gtk_entry_set_width_chars (GTK_ENTRY (command_entry), 40);
  gtk_entry_set_placeholder_text (GTK_ENTRY (command_entry), "ssh user@host");
  gtk_entry_set_activates_default (GTK_ENTRY (command_entry), TRUE);
  if (command != NULL)
    gtk_entry_set_text (GTK_ENTRY (command_entry), command);
  gtk_entry_set_visibility (GTK_ENTRY (password_entry), FALSE);
  gtk_entry_set_input_purpose (GTK_ENTRY (password_entry), GTK_INPUT_PURPOSE_PASSWORD);
  gtk_entry_set_activates_default (GTK_ENTRY (password_entry), TRUE);

  GtkWidget *hint = new_form_label (credential != NULL ?
    _("Leave the password blank to keep it if the command is unchanged. Clear the command to disable reconnecting.") :
    _("Leave the password blank to sign in manually. Saved passwords go to the system keyring."), NULL);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 46);
  gtk_label_set_max_width_chars (GTK_LABEL (status), 46);
  gtk_box_pack_start (GTK_BOX (content), new_form_label (_("SSH _command:"), command_entry), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), command_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), new_form_label (_("_Password:"), password_entry), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), password_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), hint, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), status, FALSE, FALSE, 0);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

  SshDialog *form = g_new0 (SshDialog, 1);
  g_weak_ref_init (&form->screen, screen);
  form->command = GTK_ENTRY (command_entry);
  form->password = GTK_ENTRY (password_entry);
  form->status = GTK_LABEL (status);
  g_object_set_data_full (G_OBJECT (dialog), "ssh-form", form, ssh_dialog_free);
  g_signal_connect (dialog, "response", G_CALLBACK (configure_response), form);
  g_signal_connect (command_entry, "changed", G_CALLBACK (clear_command_status), status);
  g_signal_connect_object (screen, "destroy", G_CALLBACK (gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
  gtk_widget_show_all (content);
  gtk_widget_grab_focus (command_entry);
  gtk_window_present (GTK_WINDOW (dialog));
}
