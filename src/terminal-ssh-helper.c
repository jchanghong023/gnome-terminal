/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libsecret/secret.h>
#include <uuid.h>

#include "terminal-i18n.h"

#define SSH_PID_ENV "GNOME_TERMINAL_ASKPASS_PID"
#define SSH_CREDENTIAL_ENV "GNOME_TERMINAL_ASKPASS_CREDENTIAL"

static const SecretSchema password_schema = {
  .name = "org.gnome.Terminal.SshPassword",
  .attributes = { { "credential", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } }
};

static gboolean
credential_valid (const char *credential)
{
  uuid_t uuid;
  return credential != NULL && uuid_parse (credential, uuid) == 0;
}

static int
store_password (const char *credential)
{
  char password[1023];
  size_t length = fread (password, 1, sizeof password, stdin);
  gboolean stored = FALSE;
  if (!ferror (stdin) && length > 0 && length < sizeof password &&
      memchr (password, '\0', length) == NULL && memchr (password, '\n', length) == NULL &&
      memchr (password, '\r', length) == NULL) {
    password[length] = '\0';
    stored = secret_password_store_sync (&password_schema, SECRET_COLLECTION_DEFAULT,
                                          _("GNOME Terminal SSH password"), password, NULL, NULL,
                                          "credential", credential, NULL);
  }
  for (volatile char *p = password; p < password + sizeof password; p++)
    *p = '\0';
  return stored ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int
forget_password (const char *credential)
{
  GError *error = NULL;
  secret_password_clear_sync (&password_schema, NULL, &error, "credential", credential, NULL);
  if (error == NULL)
    return EXIT_SUCCESS;
  g_error_free (error);
  return EXIT_FAILURE;
}

static gboolean
watch_parent (pid_t parent, int signal_number)
{
#ifdef __linux__
  if (prctl (PR_SET_PDEATHSIG, signal_number) < 0)
    return FALSE;
#endif
  return parent > 1 && getppid () == parent;
}

static gboolean
is_login_password_prompt (const char *prompt)
{
  const char suffix[] = "'s password: ";
  if (!g_str_has_suffix (prompt, suffix))
    return FALSE;

  const char *end = prompt + strlen (prompt) - (sizeof suffix - 1);
  const char *at = memchr (prompt, '@', end - prompt);
  if (at == NULL || at == prompt || at + 1 == end)
    return FALSE;
  for (const char *p = prompt; p < end; ++p)
    if (g_ascii_isspace (*p) || g_ascii_iscntrl (*p))
      return FALSE;
  return TRUE;
}

static gboolean
prompt_manually (const char *prompt)
{
  if (!gtk_init_check (NULL, NULL))
    return FALSE;

  const char *hint = g_getenv ("SSH_ASKPASS_PROMPT");
  if (g_strcmp0 (hint, "confirm") == 0 ||
      strstr (prompt, "Are you sure you want to continue connecting") != NULL) {
    GtkWidget *dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_CANCEL, "%s", _("Confirm SSH host key"));
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", prompt);
    gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Confirm"), GTK_RESPONSE_ACCEPT);
    gboolean accepted = gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy (dialog);
    return accepted && puts ("yes") != EOF;
  }

  GtkWidget *dialog = gtk_dialog_new_with_buttons (_("SSH Authentication"), NULL, GTK_DIALOG_MODAL,
                                                  _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                  _("_Continue"), GTK_RESPONSE_ACCEPT, NULL);
  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *label = gtk_label_new (prompt);
  GtkWidget *entry = gtk_entry_new ();
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 50);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_container_set_border_width (GTK_CONTAINER (content), 12);
  gtk_box_set_spacing (GTK_BOX (content), 6);
  gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
  gtk_entry_set_input_purpose (GTK_ENTRY (entry), GTK_INPUT_PURPOSE_PASSWORD);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), entry, FALSE, FALSE, 0);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  gtk_widget_show_all (content);
  gtk_widget_grab_focus (entry);
  gboolean accepted = gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT;
  gboolean sent = accepted && puts (gtk_entry_get_text (GTK_ENTRY (entry))) != EOF;
  gtk_entry_set_text (GTK_ENTRY (entry), "");
  gtk_widget_destroy (dialog);
  return sent;
}

static int
askpass (const char *argument)
{
  pid_t parent = getppid ();
  if (!watch_parent (parent, SIGTERM))
    return EXIT_FAILURE;

  char pid[32];
  g_snprintf (pid, sizeof pid, "%ld", (long) parent);
  const char *credential = g_getenv (SSH_CREDENTIAL_ENV);
  /* A jump host, proxy or confirmation dialog must never receive this password. */
  if (g_strcmp0 (pid, g_getenv (SSH_PID_ENV)) == 0 && credential_valid (credential) &&
      g_getenv ("SSH_ASKPASS_PROMPT") == NULL && is_login_password_prompt (argument)) {
    char *password = secret_password_lookup_sync (&password_schema, NULL, NULL,
                                                   "credential", credential, NULL);
    if (password != NULL) {
      gboolean sent = puts (password) != EOF;
      secret_password_free (password);
      return sent && fflush (stdout) != EOF ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }

  return prompt_manually (argument) && fflush (stdout) != EOF ? EXIT_SUCCESS : EXIT_FAILURE;
}

static volatile sig_atomic_t ssh_pid;

static void
forward_signal (int signo)
{
  int saved_errno = errno;
  if (ssh_pid > 0)
    kill (ssh_pid, signo);
  errno = saved_errno;
}

static int
connect_ssh (int argc, char **argv)
{
  /* Only OpenSSH can call the password responder. The user controls its arguments,
   * not its executable or the host verification and authentication policy. */
  static const char * const defaults[] = {
    "/usr/bin/ssh", "-oStrictHostKeyChecking=ask", "-oPreferredAuthentications=publickey,password",
    "-oNumberOfPasswordPrompts=1"
  };
  char **ssh_argv = g_new (char *, G_N_ELEMENTS (defaults) + argc - 3);
  for (guint i = 0; i < G_N_ELEMENTS (defaults); ++i)
    ssh_argv[i] = (char *) defaults[i];
  for (int i = 4; i <= argc; ++i)
    ssh_argv[G_N_ELEMENTS (defaults) + i - 4] = argv[i];

  static const int forwarded[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGWINCH };
  sigset_t signals, previous;
  sigemptyset (&signals);
  for (guint i = 0; i < G_N_ELEMENTS (forwarded); ++i)
    sigaddset (&signals, forwarded[i]);
  if (sigprocmask (SIG_BLOCK, &signals, &previous) < 0) {
    perror ("sigprocmask");
    return EXIT_FAILURE;
  }

  /* OpenSSH before 8.4 chooses askpass only without a controlling TTY. It
   * still uses our PTY file descriptors for the interactive remote session. */
  pid_t wrapper = getpid ();
  pid_t child = fork ();
  if (child == 0) {
    if (!watch_parent (wrapper, SIGHUP) || setsid () < 0)
      _exit (EXIT_FAILURE);

    char pid[32];
    g_snprintf (pid, sizeof pid, "%ld", (long) getpid ());
    g_setenv ("SSH_ASKPASS", argv[0], TRUE);
    g_setenv ("SSH_ASKPASS_REQUIRE", "force", TRUE);
    g_unsetenv ("SSH_ASKPASS_PROMPT");
    g_setenv (SSH_PID_ENV, pid, TRUE);
    g_setenv (SSH_CREDENTIAL_ENV, argv[2], TRUE);
    sigprocmask (SIG_SETMASK, &previous, NULL);
    execv (ssh_argv[0], ssh_argv);
    perror (ssh_argv[0]);
    _exit (EXIT_FAILURE);
  }

  g_free (ssh_argv);
  if (child < 0) {
    perror ("fork");
    return EXIT_FAILURE;
  }

  ssh_pid = child;
  struct sigaction action = { .sa_handler = forward_signal, .sa_mask = signals };
  for (guint i = 0; i < G_N_ELEMENTS (forwarded); ++i)
    sigaction (forwarded[i], &action, NULL);
  sigprocmask (SIG_SETMASK, &previous, NULL);
  int status;
  pid_t result;
  do {
    result = waitpid (child, &status, 0);
  } while (result == -1 && errno == EINTR);
  ssh_pid = 0;
  if (result != child)
    return EXIT_FAILURE;
  return WIFEXITED (status) ? WEXITSTATUS (status) : 128 + WTERMSIG (status);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  terminal_i18n_init (TRUE);

  if (argc == 2)
    return askpass (argv[1]);
  if (argc < 3 || !credential_valid (argv[2]))
    return EXIT_FAILURE;
  if (argc == 3 && g_str_equal (argv[1], "--store"))
    return store_password (argv[2]);
  if (argc == 3 && g_str_equal (argv[1], "--forget"))
    return forget_password (argv[2]);
  if (argc >= 5 && g_str_equal (argv[1], "--connect") && g_str_equal (argv[3], "ssh"))
    return connect_ssh (argc, argv);
  return EXIT_FAILURE;
}
