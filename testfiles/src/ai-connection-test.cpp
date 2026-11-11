// SPDX-License-Identifier: GPL-2.0-or-later
// JSON test driver for offline fake-CLI and loopback API integration tests.
#include "ui/dialog/ai-connection.h"
#include <chrono>
#include <iostream>
#include <iterator>
#include <thread>
using namespace Inkscape::UI::Dialog;
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--endpoints") {
        for (auto const &url : {"https://api.example.test/v1/chat/completions", "http://localhost:1234/v1/chat/completions", "http://127.0.0.1:1234/v1/chat/completions", "http://[::1]:1234/v1/chat/completions"})
            if (!connection_endpoint_valid(url)) return 1;
        for (auto const &url : {"http://example.test", "http://127.0.0.1.evil.test", "https://user:secret@example.test", "file:///tmp/model", "https://example.test/#secret", "http://0.0.0.0:1234"})
            if (connection_endpoint_valid(url)) return 1;
        std::cout << "PASS endpoint validation\n"; return 0;
    }
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    std::jthread timer;
    try {
        std::string input{std::istreambuf_iterator<char>(std::cin), {}};
        auto test = nlohmann::json::parse(input);
        AIConnection c;
        c.provider = test.at("provider"); c.endpoint = test.value("endpoint", ""); c.model = test.value("model", "test-model");
        c.key = test.value("key", ""); c.executable = test.value("executable", ""); c.images = test.value("images", false);
        c.completion_token_limit = test.value("completion_token_limit", false);
        c.timeout_seconds = test.value("timeout_seconds", c.timeout_seconds);
        c.arguments = test.value("arguments", std::vector<std::string>{});
        if (test.value("cancel", false)) timer = std::jthread([cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); g_cancellable_cancel(cancel); });
        if (test.value("async", false)) {
            struct Completion { GMainLoop *loop; std::string response, error; } completion{g_main_loop_new(nullptr, false), {}, {}};
            connection_generate_async(c, test.at("body").dump(), cancel, +[](GObject *, GAsyncResult *result, gpointer data) {
                auto &done = *static_cast<Completion *>(data);
                g_autoptr(GError) error = nullptr;
                g_autoptr(GBytes) bytes = connection_generate_finish(result, &error);
                if (error) done.error = error->message;
                else {
                    gsize size = 0;
                    auto text = static_cast<char const *>(g_bytes_get_data(bytes, &size));
                    done.response.assign(text, size);
                }
                g_main_loop_quit(done.loop);
            }, &completion);
            g_main_loop_run(completion.loop);
            g_main_loop_unref(completion.loop);
            if (!completion.error.empty()) throw std::runtime_error(completion.error);
            std::cout << completion.response;
        } else std::cout << run_connection(c, test.at("body").dump(), cancel, test.value("timeout", 0u));
    } catch (std::exception const &e) { std::cerr << e.what(); return 1; }
}
