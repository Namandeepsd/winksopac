// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone driver for ai-codex-test.py; --cancel exercises cancellation.
#include "ui/dialog/ai-codex.h"
#include <iostream>
#include <iterator>
#include <thread>
#include <chrono>
int main(int argc, char **argv) {
    auto cancel = g_cancellable_new();
    std::thread timer;
    if (argc > 1 && std::string(argv[1]) == "--cancel")
        timer = std::thread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); g_cancellable_cancel(cancel); });
    int code = 0;
    try {
        std::string request{std::istreambuf_iterator<char>(std::cin), {}};
        std::cout << Inkscape::UI::Dialog::run_codex(request, cancel);
    } catch (std::exception const &e) { std::cerr << e.what(); code = 1; }
    if (timer.joinable()) timer.join();
    g_object_unref(cancel);
    return code;
}
