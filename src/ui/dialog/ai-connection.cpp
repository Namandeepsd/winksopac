// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-connection.h"
#include "ai-codex.h"
#include <libsoup/soup.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
namespace Inkscape::UI::Dialog {
using Json = nlohmann::json;
std::vector<ConnectionPreset> const &connection_presets()
{
    static std::vector<ConnectionPreset> const presets{
        {"codex", "Codex · terminal login", ConnectionTransport::Codex, "", "codex", "codex login", "Uses the installed Codex CLI and its saved login.", true},
        {"claude", "Claude Code · terminal login", ConnectionTransport::Claude, "", "claude", "claude auth login", "Uses the installed Claude Code CLI and its saved login. Requires support for --safe-mode.", true},
        {"kimi", "Kimi · API", ConnectionTransport::Http, "https://api.moonshot.ai/v1/chat/completions", "", "", "Enter your Kimi API key and model ID. Enable image input only for a vision model.", false},
        {"ollama", "Ollama · local server", ConnectionTransport::Http, "http://localhost:11434/v1/chat/completions", "", "", "Start Ollama and enter an installed model ID. No API key is needed for the local server.", false},
        {"lmstudio", "LM Studio · local server", ConnectionTransport::Http, "http://localhost:1234/v1/chat/completions", "", "", "Start the local server in LM Studio and enter the loaded model ID.", false},
        {"http", "Custom · compatible API", ConnectionTransport::Http, "https://api.groq.com/openai/v1/chat/completions", "", "", "Use a chat-completions compatible endpoint. HTTPS or loopback HTTP; API key is optional.", false},
        {"bridge", "Custom · CLI bridge", ConnectionTransport::Bridge, "", "", "", "Choose a trusted executable that reads chat-completions JSON from stdin and writes the response JSON to stdout. Authenticate with that tool in your terminal first.", false}
    };
    return presets;
}
ConnectionPreset const &connection_preset(std::string const &id)
{
    for (auto const &preset : connection_presets()) if (preset.id == id) return preset;
    throw std::invalid_argument("Choose a supported connector in Connection settings.");
}
std::string connection_executable(AIConnection const &c)
{
    auto const &p = connection_preset(c.provider);
    auto name = c.executable.empty() ? p.executable : c.executable;
    if (name.empty()) return {};
    if (name.find('\0') != std::string::npos) return {};
    if (g_path_is_absolute(name.c_str())) return g_file_test(name.c_str(), G_FILE_TEST_IS_EXECUTABLE) &&
        !g_file_test(name.c_str(), G_FILE_TEST_IS_DIR) ? name : std::string{};
    g_autofree char *found = g_find_program_in_path(name.c_str());
    if (found) return found;
    auto local = std::string(g_get_home_dir()) + "/.local/bin/" + name;
    return name.find('/') == std::string::npos && g_file_test(local.c_str(), G_FILE_TEST_IS_EXECUTABLE) ? local : std::string{};
}
bool connection_endpoint_valid(std::string const &url)
{
    if (url.find('\0') != std::string::npos || url.find_first_of("\r\n") != std::string::npos) return false;
    g_autoptr(GUri) uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
    if (!uri || !g_uri_get_host(uri) || !*g_uri_get_host(uri) || g_uri_get_userinfo(uri) || g_uri_get_fragment(uri)) return false;
    auto host = std::string(g_uri_get_host(uri));
    return g_strcmp0(g_uri_get_scheme(uri), "https") == 0 ||
        (g_strcmp0(g_uri_get_scheme(uri), "http") == 0 && (host == "localhost" || host == "127.0.0.1" || host == "::1"));
}
void validate_connection(AIConnection const &c)
{
    auto const &p = connection_preset(c.provider);
    for (auto const &value : {c.model, c.key, c.executable})
        if (value.find('\0') != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument("Connection fields must not contain line breaks or null characters.");
    if (p.transport == ConnectionTransport::Http) {
        if (!connection_endpoint_valid(c.endpoint)) throw std::invalid_argument("Enter an HTTPS chat-completions URL, or HTTP on localhost for a local server.");
        if (c.model.empty()) throw std::invalid_argument("Enter the model ID in Connection settings.");
    } else if (connection_executable(c).empty()) throw std::invalid_argument("CLI executable not found. Install the tool or set its full path in Connection settings.");
    if (c.arguments.size() > 64) throw std::invalid_argument("Too many CLI bridge arguments.");
    for (auto const &a : c.arguments) if (a.size() > 4096 || a.find('\0') != std::string::npos) throw std::invalid_argument("Invalid CLI bridge argument.");
}
void validate_connection_request(AIConnection const &c, Json const &body)
{
    validate_connection(c);
    bool supports_images = c.images;
    auto transport = connection_preset(c.provider).transport;
    if (transport == ConnectionTransport::Codex || transport == ConnectionTransport::Claude) supports_images = true;
    for (auto const &message : body.at("messages")) {
        auto const &content = message.at("content");
        if (!content.is_array()) continue;
        for (auto const &part : content) if (part.value("type", "") == "image_url" && !supports_images)
            throw std::invalid_argument("This request includes artwork images. Choose a vision model and enable Image input in Connection settings, or use Plan without images.");
    }
}
namespace {
struct Workspace {
    std::string path;
    Workspace() {
        g_autoptr(GError) error = nullptr;
        g_autofree char *dir = g_dir_make_tmp("winkspace-connection-XXXXXX", &error);
        if (!dir) throw std::runtime_error("Could not create a private connector workspace.");
        path = dir;
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::string read_bounded(GInputStream *stream, GCancellable *cancel, std::size_t limit)
{
    std::string text;
    char buffer[8192];
    for (;;) {
        g_autoptr(GError) error = nullptr;
        auto n = g_input_stream_read(stream, buffer, sizeof(buffer), cancel, &error);
        if (n < 0) throw std::runtime_error("The connector response was interrupted.");
        if (!n) return text;
        if (text.size() + n > limit) throw std::runtime_error("The connector response exceeded its size limit.");
        text.append(buffer, n);
    }
}
std::string process(std::vector<std::string> const &args, std::string const &input, std::string const &cwd, GCancellable *cancel)
{
    std::vector<char const *> argv;
    for (auto const &arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_set_cwd(launcher, cwd.c_str());
    g_autoptr(GError) error = nullptr;
    g_autoptr(GSubprocess) child = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
    if (!child) throw std::runtime_error("Could not start the CLI. Check its executable path.");
    auto id = g_cancellable_connect(cancel, G_CALLBACK(+[](GCancellable *, gpointer child) {
        g_subprocess_force_exit(G_SUBPROCESS(child));
    }), child, nullptr);
    std::string out;
    std::exception_ptr failure;
    std::mutex guard;
    auto reader = [&](GInputStream *stream, bool output) {
        try {
            auto value = read_bounded(stream, cancel, output ? 4 * 1024 * 1024 : 256 * 1024);
            if (output) out = std::move(value);
        } catch (...) {
            { std::lock_guard lock(guard); if (!failure) failure = std::current_exception(); }
            g_subprocess_force_exit(child);
        }
    };
    std::jthread stdout_reader(reader, g_subprocess_get_stdout_pipe(child), true);
    std::jthread stderr_reader(reader, g_subprocess_get_stderr_pipe(child), false);
    auto written = g_output_stream_write_all(g_subprocess_get_stdin_pipe(child), input.data(), input.size(), nullptr, cancel, &error);
    g_output_stream_close(g_subprocess_get_stdin_pipe(child), nullptr, nullptr);
    if (!written) g_subprocess_force_exit(child);
    g_subprocess_wait(child, nullptr, nullptr);
    stdout_reader.join(); stderr_reader.join();
    if (id) g_cancellable_disconnect(cancel, id);
    if (g_cancellable_is_cancelled(cancel)) throw std::runtime_error("Request cancelled.");
    if (failure) std::rethrow_exception(failure);
    if (!written || !g_subprocess_get_successful(child))
        throw std::runtime_error("The CLI could not complete the request. Check its terminal login, model access and version; then use Test connection.");
    return out;
}
Json claude_input(Json const &body)
{
    Json content = Json::array();
    std::string text;
    unsigned images = 0;
    for (auto const &message : body.at("messages")) {
        text += "\n[" + message.at("role").get<std::string>() + "]\n";
        auto const &parts = message.at("content");
        if (parts.is_string()) { text += parts.get<std::string>(); continue; }
        for (auto const &part : parts) {
            if (part.at("type") == "text") text += part.at("text").get<std::string>();
            else if (part.at("type") == "image_url") {
                auto url = part.at("image_url").at("url").get<std::string>();
                auto comma = url.find(',');
                auto header = url.substr(0, comma);
                if (comma == std::string::npos || (header != "data:image/png;base64" && header != "data:image/jpeg;base64" && header != "data:image/webp;base64") ||
                    ++images > 8 || url.size() > 16 * 1024 * 1024 || comma + 1 == url.size()) throw std::invalid_argument("Claude requires bounded PNG, JPEG or WebP image attachments.");
                text += "\n[Attached image " + std::to_string(images) + "]\n";
                content.push_back({{"type", "text"}, {"text", text}}); text.clear();
                content.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", header.substr(5, header.size() - 12)}, {"data", url.substr(comma + 1)}}}});
            }
        }
    }
    if (!text.empty()) content.push_back({{"type", "text"}, {"text", text}});
    return {{"type", "user"}, {"message", {{"role", "user"}, {"content", content}}}};
}
std::string http(AIConnection const &c, Json body, GCancellable *cancel, unsigned timeout)
{
    // Compatible servers differ in the name of their output-token parameter.
    if (body.contains("max_completion_tokens") && !c.completion_token_limit) {
        body["max_tokens"] = body["max_completion_tokens"]; body.erase("max_completion_tokens");
    }
    g_autoptr(SoupSession) session = soup_session_new();
    soup_session_set_timeout(session, timeout);
    g_autoptr(SoupMessage) message = soup_message_new("POST", c.endpoint.c_str());
    if (!message) throw std::runtime_error("Could not create the API request.");
    soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    if (!c.key.empty()) soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", ("Bearer " + c.key).c_str());
    auto payload = body.dump();
    g_autoptr(GBytes) bytes = g_bytes_new(payload.data(), payload.size());
    soup_message_set_request_body_from_bytes(message, "application/json", bytes);
    g_autoptr(GError) error = nullptr;
    g_autoptr(GInputStream) stream = soup_session_send(session, message, cancel, &error);
    if (!stream) throw std::runtime_error("Connection failed. Check the server address and network; for local models, start the server first.");
    auto status = soup_message_get_status(message);
    if (status < 200 || status >= 300) {
        std::string reason = status == 401 || status == 403 ? "Check your API key and model access." :
            status == 404 ? "Check the chat-completions URL and model ID." :
            status == 429 ? "Rate limit or quota reached. Try again later." :
            status == 400 || status == 422 ? "Check model compatibility, image support and request parameters." :
            status >= 500 ? "The model server is temporarily unavailable." : "The provider rejected the request.";
        throw std::runtime_error("HTTP " + std::to_string(status) + ": " + reason);
    }
    return read_bounded(stream, cancel, 4 * 1024 * 1024);
}
}
std::string run_connection(AIConnection const &c, std::string const &request, GCancellable *parent, unsigned timeout)
{
    auto body = Json::parse(request);
    validate_connection_request(c, body);
    if (!timeout || timeout > 1800) throw std::invalid_argument("Invalid connector timeout.");
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    auto parent_id = parent ? g_cancellable_connect(parent, G_CALLBACK(+[](GCancellable *, gpointer target) { g_cancellable_cancel(G_CANCELLABLE(target)); }), cancel, nullptr) : 0;
    std::atomic<bool> timed_out{false};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread timer([&](std::stop_token stop) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, std::chrono::seconds(timeout), [] { return false; });
        if (!stop.stop_requested()) { timed_out = true; g_cancellable_cancel(cancel); }
    });
    bool cleaned = false;
    auto cleanup = [&] { if (cleaned) return; cleaned = true; timer.request_stop(); timer.join(); if (parent_id) g_cancellable_disconnect(parent, parent_id); };
    try {
        if (g_cancellable_is_cancelled(cancel)) throw std::runtime_error("Request cancelled.");
        std::string response;
        auto transport = connection_preset(c.provider).transport;
        if (transport == ConnectionTransport::Http) response = http(c, body, cancel, timeout);
        else if (transport == ConnectionTransport::Codex) response = run_codex(request, cancel, connection_executable(c));
        else {
            Workspace workspace;
            std::vector<std::string> args{connection_executable(c)};
            std::string input = request;
            if (transport == ConnectionTransport::Claude) {
                args.insert(args.end(), {"--print", "--safe-mode", "--tools", "", "--strict-mcp-config", "--disable-slash-commands",
                    "--no-session-persistence", "--setting-sources", "", "--settings", "{\"disableAllHooks\":true}",
                    "--input-format", "stream-json", "--output-format", "stream-json", "--verbose"});
                if (!c.model.empty()) { args.push_back("--model"); args.push_back(c.model); }
                input = claude_input(body).dump() + "\n";
            } else args.insert(args.end(), c.arguments.begin(), c.arguments.end());
            response = process(args, input, workspace.path, cancel);
            if (transport == ConnectionTransport::Claude) {
                Json result;
                std::istringstream events(response);
                std::string line;
                while (std::getline(events, line)) {
                    if (line.empty()) continue;
                    auto event = Json::parse(line);
                    if (event.value("type", "") == "result") {
                        if (!result.is_null()) throw std::runtime_error("Claude returned multiple final responses.");
                        result = std::move(event);
                    }
                }
                if (result.is_null()) throw std::runtime_error("Claude returned no final response. Check its version and terminal login.");
                if (result.value("is_error", false) || result.value("subtype", "") != "success") throw std::runtime_error("Claude could not complete the request. Check claude auth status and model access.");
                response = Json{{"choices", Json::array({{{"message", {{"content", result.at("result")}}}, {"finish_reason", "stop"}}})}}.dump();
            }
        }
        auto parsed = Json::parse(response);
        if (!parsed.at("choices").at(0).at("message").at("content").is_string()) throw std::runtime_error("The connector returned no text response.");
        if (parsed["choices"][0]["message"]["content"].get_ref<std::string const &>().empty()) throw std::runtime_error("The model returned an empty response. Try another model or a larger output limit.");
        cleanup();
        if (timed_out) throw std::runtime_error("The connection timed out. Try a shorter request or another model.");
        return response;
    } catch (...) {
        cleanup();
        if (timed_out) throw std::runtime_error("The connection timed out. Try a shorter request or another model.");
        if (parent && g_cancellable_is_cancelled(parent)) throw std::runtime_error("Request cancelled.");
        throw;
    }
}
void connection_generate_async(AIConnection c, std::string request, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
    struct Work { AIConnection connection; std::string request; };
    auto task = g_task_new(nullptr, cancel, callback, data);
    g_task_set_task_data(task, new Work{std::move(c), std::move(request)}, +[](gpointer p) { delete static_cast<Work *>(p); });
    g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancel) {
        auto &work = *static_cast<Work *>(data);
        try {
            auto response = run_connection(work.connection, work.request, cancel);
            g_task_return_pointer(task, g_bytes_new(response.data(), response.size()), reinterpret_cast<GDestroyNotify>(g_bytes_unref));
        } catch (std::exception const &e) {
            g_task_return_new_error(task, G_IO_ERROR, cancel && g_cancellable_is_cancelled(cancel) ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED, "%s", e.what());
        }
    });
    g_object_unref(task);
}
GBytes *connection_generate_finish(GAsyncResult *result, GError **error)
{
    return static_cast<GBytes *>(g_task_propagate_pointer(G_TASK(result), error));
}
}
