// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-assistant.h"
#include "preferences.h"
#include <gdkmm/clipboard.h>
#include <glibmm/i18n.h>
namespace Inkscape::UI::Dialog {
namespace { constexpr char root[] = "/dialogs/ai-assistant/connections/"; }
void AIAssistant::setup_connections()
{
    auto *title = Gtk::make_managed<Gtk::Label>(_("Model connection"));
    title->set_xalign(0); title->add_css_class("ai-popover-title");
    _settings_body.append(*title);
    for (auto const &p : connection_presets()) _connector.append(p.id, p.name);
    _settings_body.append(_connector);
    _connection_help.set_wrap(true); _connection_help.set_max_width_chars(36); _connection_help.set_xalign(0);
    _connection_help.add_css_class("dim-label"); _settings_body.append(_connection_help);
    auto field = [this](char const *caption, Gtk::Entry &entry) {
        entry.set_placeholder_text(caption); entry.set_tooltip_text(caption);
        entry.set_width_chars(24); entry.set_max_width_chars(32); entry.set_hexpand(true);
        _settings_body.append(entry);
    };
    field(_("Chat completions URL"), _endpoint);
    field(_("Model ID — blank uses the CLI default"), _model);
    field(_("API key — optional, kept in memory"), _key);
    _key.set_visibility(false); _key.set_input_purpose(Gtk::InputPurpose::PASSWORD);
    field(_("CLI executable — name or full path"), _cli_executable);
    field(_("Bridge arguments — JSON array, e.g. [\"--json\"]"), _cli_arguments);
    _connection_images.set_label(_("This model accepts image input"));
    _connection_images.set_tooltip_text(_("Enable only for a vision-capable model and endpoint. Required for editing artwork and image attachments."));
    _settings_body.append(_connection_images);
    _completion_token_limit.set_label(_("Use max_completion_tokens"));
    _completion_token_limit.set_tooltip_text(_("Enable when your API requires max_completion_tokens instead of max_tokens, for example some reasoning models."));
    _settings_body.append(_completion_token_limit);
    _connection_status.set_wrap(true); _connection_status.set_max_width_chars(36); _connection_status.set_xalign(0);
    _connection_status.set_selectable(true);
    _connection_login.set_label(_("Copy terminal sign-in command"));
    _connection_login.signal_clicked().connect([this] {
        auto const &p = connection_preset(_active_connector);
        auto executable = _cli_executable.get_text().raw();
        auto command = p.login;
        if (!executable.empty() && !p.executable.empty()) {
            g_autofree char *quoted = g_shell_quote(executable.c_str());
            command = std::string(quoted) + p.login.substr(p.executable.size());
        }
        get_clipboard()->set_text(command);
        _connection_status.set_text(_("Command copied. Run it in your terminal, finish signing in, then use Test connection."));
    });
    _connection_test.set_label(_("Test connection"));
    _connection_test.set_tooltip_text(_("Send a short text request to verify this model and connection. Provider usage may apply."));
    _connection_test.signal_clicked().connect(sigc::mem_fun(*this, &AIAssistant::test_connection));
    auto *shell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto *footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    footer->set_margin(12);
    footer->append(_connection_status);
    footer->append(_connection_login);
    footer->append(_connection_test);
    shell->append(_settings_scroll);
    shell->append(*footer);
    _settings.set_child(*shell);
    auto prefs = Inkscape::Preferences::get();
    for (auto const &p : connection_presets()) {
        auto path = std::string(root) + p.id;
        AIConnection c;
        c.provider = p.id;
        c.endpoint = prefs->getString(path + "/endpoint", p.endpoint);
        c.model = prefs->getString(path + "/model");
        c.executable = prefs->getString(path + "/executable", p.executable);
        c.images = prefs->getBool(path + "/images", p.images);
        c.completion_token_limit = prefs->getBool(path + "/completion-token-limit", false);
        _connection_profiles[p.id] = std::move(c);
    }
    auto changed = [this] { if (!_loading_connection) { save_connection(); refresh_connection_status(); } };
    for (auto entry : {&_endpoint, &_model, &_key, &_cli_executable, &_cli_arguments}) entry->signal_changed().connect(changed);
    _connection_images.signal_toggled().connect(changed);
    _completion_token_limit.signal_toggled().connect(changed);
    _connector.signal_changed().connect(sigc::mem_fun(*this, &AIAssistant::change_connection));
    AIConnection codex; codex.provider = "codex";
    auto selected = prefs->getString(std::string(root) + "selected", connection_executable(codex).empty() ? "http" : "codex");
    try { connection_preset(selected); } catch (...) { selected = "http"; }
    _connector.set_active_id(selected);
}
AIConnection AIAssistant::selected_connection() const
{
    AIConnection c;
    c.provider = _active_connector; c.endpoint = _endpoint.get_text(); c.model = _model.get_text(); c.key = _key.get_text();
    c.executable = _cli_executable.get_text(); c.images = _connection_images.get_active();
    c.completion_token_limit = _completion_token_limit.get_active();
    if (connection_preset(c.provider).transport == ConnectionTransport::Bridge) {
        auto text = _cli_arguments.get_text().raw();
        if (!text.empty()) {
            try { c.arguments = nlohmann::json::parse(text).get<std::vector<std::string>>(); }
            catch (...) { throw std::invalid_argument("Bridge arguments must be a JSON array of strings, for example [\"--json\"]."); }
        }
    }
    return c;
}
void AIAssistant::save_connection()
{
    if (_active_connector.empty() || _loading_connection) return;
    auto &c = _connection_profiles[_active_connector];
    c.provider = _active_connector; c.endpoint = _endpoint.get_text(); c.model = _model.get_text(); c.key = _key.get_text();
    c.executable = _cli_executable.get_text(); c.images = _connection_images.get_active();
    c.completion_token_limit = _completion_token_limit.get_active();
    // Bridge arguments may carry tool-specific credentials, so keep them session-local.
    try { c.arguments = selected_connection().arguments; } catch (...) {}
    auto path = std::string(root) + _active_connector;
    auto prefs = Inkscape::Preferences::get();
    if (connection_endpoint_valid(c.endpoint)) prefs->setString(path + "/endpoint", c.endpoint);
    prefs->setString(path + "/model", c.model);
    prefs->setString(path + "/executable", c.executable);
    prefs->setBool(path + "/images", c.images);
    prefs->setBool(path + "/completion-token-limit", c.completion_token_limit);
}
void AIAssistant::change_connection()
{
    save_connection();
    _loading_connection = true;
    _active_connector = _connector.get_active_id();
    auto const &p = connection_preset(_active_connector);
    auto const &c = _connection_profiles.at(_active_connector);
    _endpoint.set_text(c.endpoint); _model.set_text(c.model); _key.set_text(c.key);
    _cli_executable.set_text(c.executable); _cli_arguments.set_text(nlohmann::json(c.arguments).dump());
    _connection_images.set_active(p.transport == ConnectionTransport::Codex || p.transport == ConnectionTransport::Claude || c.images);
    _completion_token_limit.set_active(c.completion_token_limit);
    bool http = p.transport == ConnectionTransport::Http;
    _endpoint.set_visible(http); _key.set_visible(http); _cli_executable.set_visible(!http);
    _completion_token_limit.set_visible(http);
    _cli_arguments.set_visible(p.transport == ConnectionTransport::Bridge);
    _connection_images.set_sensitive(http || p.transport == ConnectionTransport::Bridge);
    _connection_login.set_visible(!p.login.empty());
    _model.set_placeholder_text(http ? _("Model ID") : _("Model ID — blank uses the CLI default"));
    _connection_help.set_text(p.help);
    _settings_button.set_tooltip_text(_("Connection: ") + p.name);
    Inkscape::Preferences::get()->setString(std::string(root) + "selected", _active_connector);
    _loading_connection = false;
    refresh_connection_status();
}
void AIAssistant::refresh_connection_status()
{
    try {
        auto c = selected_connection();
        validate_connection(c);
        auto transport = connection_preset(c.provider).transport;
        _connection_status.set_text(transport == ConnectionTransport::Http ?
            _("Configured. Test the connection to verify model access.") :
            _("CLI found. Test the connection to verify your saved login."));
    } catch (std::exception const &e) { _connection_status.set_text(e.what()); }
}
void AIAssistant::test_connection()
{
    if (_cancellable || _tracing || workflow_busy()) return;
    try {
        auto connection = selected_connection(); validate_connection(connection);
        nlohmann::json body = {{"model", connection.model}, {"stream", false}, {"max_completion_tokens", 256},
            {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "Reply with OK only."}}})}};
        _cancellable = g_cancellable_new();
        set_busy(true);
        _connection_status.set_text(_("Testing connection…"));
        set_status(_("Testing model connection…"), false);
        struct Check { AIAssistant *panel; std::weak_ptr<int> alive; };
        auto check = new Check{this, _alive};
        connection_generate_async(connection, body.dump(), _cancellable, +[](GObject *, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Check> check(static_cast<Check *>(data));
            g_autoptr(GError) error = nullptr;
            g_autoptr(GBytes) bytes = connection_generate_finish(result, &error);
            if (check->alive.expired()) return;
            auto panel = check->panel;
            g_clear_object(&panel->_cancellable);
            panel->set_busy(false);
            std::string status = error ? error->message : _("Connected. The selected model responded successfully. Image support was not tested.");
            panel->_connection_status.set_text(status);
            panel->set_status(status, error != nullptr);
        }, check);
    } catch (std::exception const &e) { _connection_status.set_text(e.what()); }
}
}
