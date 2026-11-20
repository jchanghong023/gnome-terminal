/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TERMINAL_SSH_H
#define TERMINAL_SSH_H

#include "terminal-screen.h"

char **terminal_ssh_build_argv (const char *command,
                                const char *credential,
                                const char *remote_directory,
                                const char *login_command,
                                gboolean require_directory,
                                GError **error);
gboolean terminal_ssh_command_accepts_remote_startup (const char *command);
/* Serialize SSH arguments without changing their boundaries or order. */
char *terminal_ssh_join_command (char **argv);
char *terminal_ssh_canonicalize_command (const char *command);
void terminal_ssh_configure (GtkWindow *parent,
                             TerminalScreen *screen);
void terminal_ssh_forget_password (const char *credential);
gboolean terminal_ssh_remove_password (const char *credential);
char *terminal_ssh_dup_password (const char *credential, GError **error);

#endif
