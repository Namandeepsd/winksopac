// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-codex.h"
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <glib/gstdio.h>
#include <nlohmann/json.hpp>

namespace Inkscape::UI::Dialog {
namespace {
using Json = nlohmann::json;
struct Workspace {
    std::string path;
    Workspace() {
        g_autoptr(GError) error = nullptr;
        g_autofree char *directory = g_dir_make_tmp("inkscape-codex-XXXXXX", &error);
        if (!directory) throw std::runtime_error(error->message);
        path = directory;
    }
    ~Workspace() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
void write_file(std::string const &path, char const *data, gsize length) {
    g_autoptr(GError) error = nullptr;
    if (!g_file_set_contents(path.c_str(), data, length, &error)) throw std::runtime_error(error->message);
}
}

std::string codex_executable() {
    g_autofree char *program = g_find_program_in_path("codex");
    if (program) return program;
    // Desktop launchers may omit ~/.local/bin from PATH.
    auto local = std::string(g_get_home_dir()) + "/.local/bin/codex";
    return g_file_test(local.c_str(), G_FILE_TEST_IS_EXECUTABLE) ? local : std::string{};
}

std::string run_codex(std::string const &request, GCancellable *cancel, std::string executable) {
    if (executable.empty()) executable = codex_executable();
    if (executable.empty()) throw std::runtime_error("Codex CLI was not found. Install Codex and run codex login first.");
    auto body = Json::parse(request);
    Workspace workspace;
    auto output = workspace.path + "/result.txt";
    std::vector<std::string> args{executable, "exec", "--ignore-user-config", "--ephemeral",
        "--skip-git-repo-check", "--sandbox", "read-only", "--color", "never",
        "-c", "features.shell_tool=false", "-c", "features.multi_agent=false",
        "-c", "features.apps=false", "-C", workspace.path, "--output-last-message", output};
    auto model = body.value("model", std::string{});
    if (!model.empty()) { args.push_back("--model"); args.push_back(model); }
    std::string prompt = "You are the artwork engine for Inkscape. Return only the requested edit JSON or SVG. "
        "Do not modify files or use tools. The host renders a preview and applies edits after user review.\n";
    unsigned images = 0;
    for (auto const &message : body.at("messages")) {
        prompt += "\n[" + message.at("role").get<std::string>() + "]\n";
        auto const &content = message.at("content");
        if (content.is_string()) { prompt += content.get<std::string>() + "\n"; continue; }
        for (auto const &part : content) {
            if (part.at("type") == "text") prompt += part.at("text").get<std::string>() + "\n";
            else if (part.at("type") == "image_url") {
                auto url = part.at("image_url").at("url").get<std::string>();
                auto comma = url.find(',');
                auto header = url.substr(0, comma);
                if (header != "data:image/png;base64" && header != "data:image/jpeg;base64" && header != "data:image/webp;base64")
                    throw std::runtime_error("Local Codex requires attached PNG, JPEG or WebP images.");
                if (++images > 8 || url.size() > 16 * 1024 * 1024) throw std::runtime_error("Too many or oversized Codex images.");
                gsize length = 0;
                g_autofree guchar *bytes = g_base64_decode(url.c_str() + comma + 1, &length);
                if (!length) throw std::runtime_error("Empty Codex image attachment.");
                auto path = workspace.path + "/image-" + std::to_string(images) +
                    (header == "data:image/png;base64" ? ".png" : header == "data:image/jpeg;base64" ? ".jpg" : ".webp");
                write_file(path, reinterpret_cast<char const *>(bytes), length);
                args.push_back("--image"); args.push_back(path);
                prompt += "[Attached image " + std::to_string(images) + "]\n";
            }
        }
    }
    args.push_back("-"); // Prompt goes through stdin, never through a shell or process arguments.
    std::vector<char const *> argv;
    for (auto const &arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    g_autoptr(GError) error = nullptr;
    g_autoptr(GSubprocess) process = g_subprocess_newv(argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
    if (!process) throw std::runtime_error(std::string("Could not start Codex: ") + error->message);
    auto connection = cancel ? g_cancellable_connect(cancel, G_CALLBACK(+[](GCancellable *, gpointer data) {
        g_subprocess_force_exit(G_SUBPROCESS(data));
    }), process, nullptr) : 0;
    g_autofree char *diagnostic = nullptr;
    auto success = g_subprocess_communicate_utf8(process, prompt.c_str(), cancel, nullptr, &diagnostic, &error);
    if (connection) g_cancellable_disconnect(cancel, connection);
    if (!success) {
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, nullptr, nullptr);
        throw std::runtime_error(error ? error->message : "Codex was interrupted.");
    }
    if (!g_subprocess_get_successful(process)) {
        std::string detail = diagnostic ? diagnostic : "";
        if (detail.size() > 1800) detail = detail.substr(detail.size() - 1800);
        throw std::runtime_error("Codex could not complete the edit. Check codex login status and your connection. " + detail);
    }
    if (std::filesystem::file_size(output) > 4 * 1024 * 1024) throw std::runtime_error("Codex response exceeded the size limit.");
    g_autofree char *text = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(output.c_str(), &text, &length, &error) || !length)
        throw std::runtime_error("Codex returned no final edit.");
    return Json{{"choices", Json::array({{{"message", {{"content", std::string(text, length)}}}, {"finish_reason", "stop"}}})}}.dump();
}

void codex_generate_async(std::string request, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
    auto task = g_task_new(nullptr, cancel, callback, data);
    g_task_set_task_data(task, new std::string(std::move(request)), +[](gpointer p) { delete static_cast<std::string *>(p); });
    g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancel) {
        try {
            auto result = run_codex(*static_cast<std::string *>(data), cancel);
            g_task_return_pointer(task, g_bytes_new(result.data(), result.size()), reinterpret_cast<GDestroyNotify>(g_bytes_unref));
        } catch (std::exception const &e) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what());
        }
    });
    g_object_unref(task);
}
GBytes *codex_generate_finish(GAsyncResult *result, GError **error) {
    return static_cast<GBytes *>(g_task_propagate_pointer(G_TASK(result), error));
}
}
