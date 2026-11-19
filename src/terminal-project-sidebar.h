/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TERMINAL_PROJECT_SIDEBAR_H
#define TERMINAL_PROJECT_SIDEBAR_H

#include "terminal-mdi-container.h"
#include "terminal-window.h"

GtkWidget *terminal_project_sidebar_new (TerminalWindow *window,
                                         TerminalMdiContainer *container);

#endif
