/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <string.h>
#include <sysexits.h>
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

/* Only add a remote command to shell connections, not explicit commands or
 * non-interactive modes. OpenSSH also accepts options after its destination. */
static gboolean
is_shell_connection (char **argv, int argc)
{
  gboolean destination = FALSE, options = TRUE;
  for (int i = 1; i < argc; ++i) {
    if (options && g_str_equal (argv[i], "--")) {
      options = FALSE;
      continue;
    }
    if (!options || argv[i][0] != '-' || argv[i][1] == '\0') {
      if (destination || g_str_equal (argv[i], "-"))
        return FALSE;
      destination = TRUE;
      continue;
    }

    for (const char *option = argv[i] + 1; *option != '\0'; ++option) {
      if (strchr ("fGnNOQPsTVW", *option) != NULL)
        return FALSE;
      if (strchr ("BbcDEeFIiJLlmopRSw", *option) != NULL) {
        const char *value = option + 1;
        if (*value == '\0') {
          if (++i == argc)
            return FALSE;
          value = argv[i];
        }
        if (*option == 'o') {
          const char *key = value + strspn (value, " \t");
          gsize len = strcspn (key, " \t=");
          if (len == strlen ("RemoteCommand") &&
              g_ascii_strncasecmp (key, "RemoteCommand", len) == 0)
            return FALSE;
        }
        break;
      }
    }
  }
  return destination;
}

gboolean
terminal_ssh_command_accepts_remote_startup (const char *command)
{
  gs_strfreev char **argv = NULL;
  int argc;
  return command != NULL && g_shell_parse_argv (command, &argc, &argv, NULL) &&
         g_str_equal (argv[0], "ssh") && is_shell_connection (argv, argc);
}

char *
terminal_ssh_join_command (char **argv)
{
  GString *command = g_string_new ("ssh");
  for (char **arg = argv + 1; *arg != NULL; ++arg) {
    if (!g_utf8_validate (*arg, -1, NULL)) {
      g_string_free (command, TRUE);
      return NULL;
    }
    g_string_append_c (command, ' ');
    if (**arg != '\0' &&
        (*arg)[strspn (*arg, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./@%+=,:-")] == '\0') {
      g_string_append (command, *arg);
    } else {
      gs_free char *quoted = g_shell_quote (*arg);
      g_string_append (command, quoted);
    }
  }
  return g_string_free (command, FALSE);
}

char *
terminal_ssh_canonicalize_command (const char *command)
{
  gs_strfreev char **argv = NULL;
  return command != NULL && g_shell_parse_argv (command, NULL, &argv, NULL) &&
         g_str_equal (argv[0], "ssh") ? terminal_ssh_join_command (argv) : NULL;
}

/* The server's login shell parses the remote command before /bin/sh.
 * Passing only octal bytes keeps input literal under both sh and csh. */
static void
append_remote_argument (GString *command,
                        const char *value)
{
  g_string_append (command, " '");
  if (value != NULL)
    for (const guchar *p = (const guchar *) value; *p != '\0'; ++p)
      g_string_append_printf (command, "\\0%03o", (unsigned int) *p);
  g_string_append_c (command, '\'');
}

static char *
remote_shell_command (const char *directory,
                      const char *login_command,
                      gboolean require_directory)
{
  GString *command = g_string_new ("exec /bin/sh -c '"
                                   "if [ -n \"$1\" ]; then "
                                   "directory=$(printf \"%b.\" \"$1\"); "
                                   "if ! cd -P \"${directory%.}\"; then ");
  g_string_append (command, require_directory ? "exit 1; " : "cd \"$HOME\"; set -- \"$1\" \"\"; ");
  g_string_append (command, "fi; fi; "
                                   "SHELL=${SHELL:-/bin/sh}; VTE_VERSION=5200; export SHELL VTE_VERSION; "
                                   "if [ -n \"$2\" ]; then "
                                   "startup=$(printf \"%b\\nexec \\\"\\$SHELL\\\" -l.\" \"$2\"); "
                                   "exec \"$SHELL\" -c \"${startup%.}\"; fi; "
                                   "exec \"$SHELL\" -l' sh");
  append_remote_argument (command, directory);
  append_remote_argument (command, login_command);
  return g_string_free (command, FALSE);
}

char **
terminal_ssh_build_argv (const char *command,
                         const char *credential,
                         const char *remote_directory,
                         const char *login_command,
                         gboolean require_directory,
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

  gboolean set_directory = remote_directory != NULL && *remote_directory != '\0';
  gboolean run_command = login_command != NULL && *login_command != '\0';
  if (set_directory || run_command) {
    if ((set_directory && !g_path_is_absolute (remote_directory)) || !is_shell_connection (argv, argc)) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           _("A remote directory or login command requires an SSH connection without a remote command and an absolute directory if specified."));
      return NULL;
    }
    argv = g_realloc_n (argv, argc + 3, sizeof *argv);
    memmove (argv + 2, argv + 1, (argc - 1) * sizeof *argv);
    argv[1] = g_strdup ("-t");
    argv[argc + 1] = remote_shell_command (remote_directory, login_command, require_directory);
    argc += 2;
    argv[argc] = NULL;
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
run_password_helper (const char *operation,
                     const char *credential,
                     gboolean store)
{
  gs_free char *helper = helper_path ();
  const char *argv[] = { helper, operation, credential, NULL };
  return g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                  G_SUBPROCESS_FLAGS_STDERR_SILENCE |
                                  (store ? G_SUBPROCESS_FLAGS_STDIN_PIPE : 0), NULL);
}

void
terminal_ssh_forget_password (const char *credential)
{
  gs_unref_object GSubprocess *process = run_password_helper ("--forget", credential, FALSE);
}

gboolean
terminal_ssh_remove_password (const char *credential)
{
  gs_unref_object GSubprocess *process = run_password_helper ("--forget", credential, FALSE);
  return process != NULL && g_subprocess_wait_check (process, NULL, NULL);
}

char *
terminal_ssh_dup_password (const char *credential,
                           GError **error)
{
  if (credential == NULL)
    return NULL;

  uuid_t uuid;
  char copy[37];
  uuid_generate (uuid);
  uuid_unparse (uuid, copy);
  gs_free char *helper = helper_path ();
  const char *argv[] = { helper, "--copy", credential, copy, NULL };
  gs_unref_object GSubprocess *process =
    g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
  if (process == NULL)
    return NULL;
  if (!g_subprocess_wait_check (process, NULL, NULL)) {
    if (g_subprocess_get_if_exited (process) &&
        g_subprocess_get_exit_status (process) == EX_NOINPUT) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           _("The saved SSH password is no longer available."));
      return NULL;
    }
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("Could not copy the saved SSH password. Enter it again in SSH on Reopen."));
    return NULL;
  }
  return g_strdup (copy);
}

typedef struct {
  GWeakRef screen;
  GtkEntry *command;
  GtkEntry *directory;
  GtkEntry *login_command;
  GtkEntry *password;
  GtkLabel *status;
  gboolean directory_edited;
} SshDialog;

static void
ssh_dialog_free (gpointer data)
{
  SshDialog *form = data;
  g_weak_ref_clear (&form->screen);
  g_free (form);
}

/* Commit a new password before returning from Save: closing the window
 * immediately must neither miss the edited settings nor orphan the file. */
static gboolean
store_password (const char *credential,
                const char *password)
{
  gs_unref_object GSubprocess *process = run_password_helper ("--store", credential, TRUE);
  if (process == NULL)
    return FALSE;
  gs_unref_bytes GBytes *input = g_bytes_new_static (password, strlen (password));
  gboolean sent = g_subprocess_communicate (process, input, NULL, NULL, NULL, NULL);
  if (g_subprocess_wait_check (process, NULL, NULL) && sent)
    return TRUE;
  terminal_ssh_forget_password (credential);
  return FALSE;
}

static void
configure_response (GtkDialog *dialog,
                     int response,
                     SshDialog *form)
{
  if (response != GTK_RESPONSE_ACCEPT && response != GTK_RESPONSE_REJECT) {
    gtk_widget_destroy (GTK_WIDGET (dialog));
    return;
  }

  gs_unref_object TerminalScreen *screen = g_weak_ref_get (&form->screen);
  if (screen == NULL) {
    gtk_widget_destroy (GTK_WIDGET (dialog));
    return;
  }

  if (response == GTK_RESPONSE_REJECT) {
    if (!terminal_screen_forget_ssh_password (screen))
      gtk_label_set_text (form->status, _("Could not remove the saved SSH password."));
    else
      gtk_widget_destroy (GTK_WIDGET (dialog));
    return;
  }

  const char *old_credential = NULL;
  const char *old_command = terminal_screen_get_startup_ssh (screen, &old_credential);
  gs_free char *command = g_strstrip (g_strdup (gtk_entry_get_text (form->command)));
  gs_free char *canonical = terminal_ssh_canonicalize_command (command);
  gboolean keep = g_strcmp0 (canonical, old_command) == 0;
  const char *directory = form->directory_edited ?
    gtk_entry_get_text (form->directory) : keep ? terminal_screen_get_startup_ssh_directory (screen) : NULL;
  const char *login_command = gtk_entry_get_text (form->login_command);
  const char *password = gtk_entry_get_text (form->password);
  gboolean store = *command != '\0' && *password != '\0';
  if (*command == '\0') {
    directory = NULL;
    login_command = NULL;
  }
  gs_free char *reported = keep && !form->directory_edited ?
    terminal_screen_dup_remote_directory (screen) : NULL;

  if (!store && *password != '\0') {
    gtk_label_set_text (form->status, _("Enter an SSH command before saving a password."));
    return;
  }

  if (command != NULL && *command != '\0') {
    gs_free_error GError *error = NULL;
    gs_strfreev char **argv = terminal_ssh_build_argv (command, NULL,
                                                     reported != NULL ? reported : directory,
                                                     login_command, FALSE, &error);
    if (argv == NULL) {
      gtk_label_set_text (form->status, error->message);
      return;
    }
  }

  if (store && (strlen (password) > 1022 || strpbrk (password, "\r\n") != NULL)) {
    gtk_label_set_text (form->status, _("Enter a password of up to 1022 bytes without line breaks."));
    return;
  }

  char new_credential[37];
  if (store) {
    uuid_t uuid;
    uuid_generate (uuid);
    uuid_unparse (uuid, new_credential);
    gboolean stored = store_password (new_credential, password);
    gtk_entry_set_text (form->password, "");
    if (!stored) {
      gtk_label_set_text (form->status, _("Could not update the password in your home directory."));
      return;
    }
  }

  terminal_screen_set_startup_ssh (screen, canonical, store ? new_credential : keep ? old_credential : NULL);
  if (form->directory_edited || !keep)
    terminal_screen_set_startup_ssh_directory (screen, directory);
  terminal_screen_set_ssh_login_command (screen, login_command);
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
clear_command_status (GtkEditable *entry,
                      GtkLabel *status)
{
  gtk_label_set_text (status, "");
}

static void
edit_directory (GtkEditable *entry,
                GtkDialog *dialog)
{
  SshDialog *form = g_object_get_data (G_OBJECT (dialog), "ssh-form");
  form->directory_edited = TRUE;
  gtk_label_set_text (form->status, "");
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
  gs_free char *reported_directory = terminal_screen_dup_remote_directory (screen);
  const char *directory = reported_directory != NULL ? reported_directory :
    terminal_screen_get_startup_ssh_directory (screen);
  const char *login_command = terminal_screen_get_ssh_login_command (screen);
  const char *command = terminal_screen_get_startup_ssh (screen, &credential);
  const char *active_credential = NULL;
  terminal_screen_get_active_ssh (screen, &active_credential, NULL);
  GtkWidget *dialog = gtk_dialog_new_with_buttons (_("SSH on Reopen"), parent,
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                   _("_Save"), GTK_RESPONSE_ACCEPT,
                                                   NULL);
  if (credential != NULL || active_credential != NULL)
    gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Forget Password"), GTK_RESPONSE_REJECT);

  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *command_entry = gtk_entry_new ();
  GtkWidget *directory_entry = gtk_entry_new ();
  GtkWidget *login_entry = gtk_entry_new ();
  GtkWidget *password_entry = gtk_entry_new ();
  GtkWidget *status = new_form_label ("", NULL);
  gtk_container_set_border_width (GTK_CONTAINER (content), 12);
  gtk_box_set_spacing (GTK_BOX (content), 6);
  gtk_entry_set_width_chars (GTK_ENTRY (command_entry), 40);
  gtk_entry_set_placeholder_text (GTK_ENTRY (command_entry), "ssh user@host");
  gtk_entry_set_activates_default (GTK_ENTRY (command_entry), TRUE);
  if (command != NULL)
    gtk_entry_set_text (GTK_ENTRY (command_entry), command);
  gtk_entry_set_placeholder_text (GTK_ENTRY (directory_entry), _("/absolute/remote/path"));
  gtk_entry_set_activates_default (GTK_ENTRY (directory_entry), TRUE);
  if (directory != NULL)
    gtk_entry_set_text (GTK_ENTRY (directory_entry), directory);
  gtk_entry_set_placeholder_text (GTK_ENTRY (login_entry), _("cd /remote/project"));
  gtk_entry_set_activates_default (GTK_ENTRY (login_entry), TRUE);
  if (login_command != NULL)
    gtk_entry_set_text (GTK_ENTRY (login_entry), login_command);
  gtk_entry_set_visibility (GTK_ENTRY (password_entry), FALSE);
  gtk_entry_set_input_purpose (GTK_ENTRY (password_entry), GTK_INPUT_PURPOSE_PASSWORD);
  gtk_entry_set_activates_default (GTK_ENTRY (password_entry), TRUE);

  GtkWidget *hint = new_form_label (credential != NULL ?
    _("Leave the password blank to keep it if the command is unchanged. Clear the command to disable reconnecting.") :
    _("Leave the password blank to sign in manually. Saved passwords are stored as plain text in ~/.gnome-terminal-ssh/."),
    NULL);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 46);
  gtk_label_set_max_width_chars (GTK_LABEL (status), 46);
  gtk_box_pack_start (GTK_BOX (content), new_form_label (_("SSH _command:"), command_entry), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), command_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), new_form_label (_("Remote _directory (optional):"), directory_entry), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), directory_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), new_form_label (_("_Password:"), password_entry), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), password_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), new_form_label (_("Command after _login (optional):"), login_entry), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), login_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), hint, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), status, FALSE, FALSE, 0);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

  SshDialog *form = g_new0 (SshDialog, 1);
  g_weak_ref_init (&form->screen, screen);
  form->command = GTK_ENTRY (command_entry);
  form->directory = GTK_ENTRY (directory_entry);
  form->password = GTK_ENTRY (password_entry);
  form->login_command = GTK_ENTRY (login_entry);
  form->status = GTK_LABEL (status);
  g_object_set_data_full (G_OBJECT (dialog), "ssh-form", form, ssh_dialog_free);
  g_signal_connect (dialog, "response", G_CALLBACK (configure_response), form);
  g_signal_connect_object (command_entry, "changed", G_CALLBACK (clear_command_status), status, 0);
  g_signal_connect_object (directory_entry, "changed", G_CALLBACK (edit_directory), dialog, 0);
  g_signal_connect_object (login_entry, "changed", G_CALLBACK (clear_command_status), status, 0);
  g_signal_connect_object (screen, "destroy", G_CALLBACK (gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
  gtk_widget_show_all (content);
  gtk_widget_grab_focus (command_entry);
  gtk_window_present (GTK_WINDOW (dialog));
}
