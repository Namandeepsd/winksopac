// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_CODEX_H
#define INKSCAPE_UI_DIALOG_AI_CODEX_H
#include <gio/gio.h>
#include <string>
namespace Inkscape::UI::Dialog {
// Uses the CLI's own saved login. No credentials are read by Inkscape.
std::string codex_executable();
std::string run_codex(std::string const &request, GCancellable *cancel, std::string executable = {});
void codex_generate_async(std::string request, GCancellable *cancel,
                          GAsyncReadyCallback callback, gpointer data);
GBytes *codex_generate_finish(GAsyncResult *result, GError **error);
}
#endif
