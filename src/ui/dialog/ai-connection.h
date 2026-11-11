// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_CONNECTION_H
#define INKSCAPE_UI_DIALOG_AI_CONNECTION_H
#include <gio/gio.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace Inkscape::UI::Dialog {
enum class ConnectionTransport { Http, Codex, Claude, Bridge };
struct ConnectionPreset {
    std::string id, name;
    ConnectionTransport transport;
    std::string endpoint, executable, login, help;
    bool images;
};
struct AIConnection {
    std::string provider = "codex", endpoint, model, key, executable;
    std::vector<std::string> arguments;
    bool images = false;
    bool completion_token_limit = false;
};
std::vector<ConnectionPreset> const &connection_presets();
ConnectionPreset const &connection_preset(std::string const &id);
std::string connection_executable(AIConnection const &);
bool connection_endpoint_valid(std::string const &);
void validate_connection(AIConnection const &);
void validate_connection_request(AIConnection const &, nlohmann::json const &);
// Same chat-completions envelope for every transport; no canvas/widget access.
std::string run_connection(AIConnection const &, std::string const &request, GCancellable *, unsigned timeout = 120);
void connection_generate_async(AIConnection, std::string request, GCancellable *, GAsyncReadyCallback, gpointer);
GBytes *connection_generate_finish(GAsyncResult *, GError **);
}
#endif
