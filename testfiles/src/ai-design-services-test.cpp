// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/dialog/ai-design-services.h"
#include "util/statics.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <glib.h>
using namespace Inkscape::AI;
using namespace Inkscape::UI::Dialog;
using namespace std::chrono_literals;
namespace {
void check(bool ok, char const *message) { if (!ok) throw std::runtime_error(message); }
void wait(std::shared_ptr<DesignRun> const &run)
{
    auto limit = Clock::now() + 5s;
    while (!run->done && Clock::now() < limit) std::this_thread::sleep_for(1ms);
    if (!run->done) { run->workflow->cancel(); throw std::runtime_error("Session did not finish"); }
}
}
int main(int argc, char **argv)
{
    Inkscape::Util::Statics statics;
    try {
        if (argc > 1 && (std::string(argv[1]) == "--live-search" || std::string(argv[1]) == "--live-assets")) {
            auto bus = std::make_shared<MessageBus>(); bus->register_tasks({"search"});
            std::stop_source stop;
            ExecutionContext execution("search", 1, bus, stop.get_token(), Clock::now() + 90s);
            auto result = search_commons("solar cell diagram", execution);
            std::cout << "Commons search returned " << result.size() << " verified source records\n";
            for (auto const &entry : result) std::cout << entry.at("source").get<std::string>() << '\n';
            if (std::string(argv[1]) == "--live-assets") {
                unsigned available = 0;
                for (auto const &entry : result) {
                    std::cout << "Download: " << entry.value("download", std::string{}) << "\nThumbnail: " << entry.value("thumbnail", std::string{}) << '\n';
                    auto asset = prepare_design_asset(entry, "asset-1", download_commons_asset, execution);
                    std::cout << asset.title << ": " << (asset.available() ? asset.svg.empty() ? "embedded PNG ready" : "editable SVG ready" : asset.error) << '\n';
                    if (asset.available()) {
                        ++available;
                        auto composition = compose_design_assets("<svg xmlns='http://www.w3.org/2000/svg' width='800' height='600'><rect id='asset-1' x='20' y='20' width='760' height='560'/></svg>", {asset});
                        std::ofstream("/tmp/ai-live-asset.svg") << composition;
                        std::ofstream("/tmp/ai-live-asset.png", std::ios::binary).write(asset.png.data(), asset.png.size());
                        break;
                    }
                }
                check(available, "No live image could be prepared");
            }
            return 0;
        }
        Json plan = {{"title", "Solar poster"}, {"summary", "Explain photovoltaics"}, {"style", "Clean"},
            {"palette", Json::array()}, {"layout", Json::array({{{"section", "Overview"}, {"content", "[Data to be supplied]"}}})},
            {"asset_queries", Json::array({{{"query", "solar cell"}, {"purpose", "Illustration reference"}}})}};
        DesignModel model = [plan](auto const &, auto const &, Json const &input, auto &) {
            return input.contains("request") ? plan.dump() : Json{{"indices", Json::array({0})}}.dump();
        };
        AssetSearch search = [](auto const &, auto &) { return Json::array({{{"title", "File:Test.svg"},
            {"source", "https://commons.wikimedia.org/wiki/File:Test.svg"}, {"license", "CC0"}, {"attribution", "Test"}}}); };
        g_autofree char *directory = g_dir_make_tmp("inkscape-design-session-XXXXXX", nullptr);
        check(directory, "Cannot create temporary fixture");
        auto path = std::string(directory) + "/design";
        auto session = DesignRun::start({}, "project", path, "Solar poster", "Minimal", {{"width", 800}}, model, search);
        wait(session);
        check(session->error.empty() && session->assets.size() == 1 && !session->plan.is_null(), "Session did not collect agent results");
        check(std::filesystem::exists(path + ".context.json") && std::filesystem::exists(path + ".outputs.json"), "Session did not persist context and outputs");
        auto repeat = DesignRun::start({}, "project", path, "New iteration", "Minimal", {}, model, search);
        wait(repeat);
        check(repeat->error.empty() && repeat->outputs->list().size() == 4, "Persisted outputs were not retained across iterations");
        std::ofstream(path + ".context.json") << "corrupted";
        auto corrupt = DesignRun::start({}, "project", path, "Keep working", "", {}, model, search);
        wait(corrupt);
        check(!corrupt->warning.empty() && corrupt->error.empty(), "Storage failure was not surfaced independently of design");
        std::string original; { std::ifstream file(path + ".context.json"); file >> original; }
        check(original == "corrupted", "Damaged storage was overwritten");
        std::atomic<bool> entered{false};
        DesignModel slow = [&entered](auto const &, auto const &, auto const &, ExecutionContext &execution) -> std::string {
            entered = true;
            for (;;) { execution.checkpoint(); std::this_thread::sleep_for(1ms); }
        };
        auto active = DesignRun::start({}, "overlap-project", path + "-overlap", "First panel", "", {}, slow, search);
        auto deadline = Clock::now() + 5s;
        while (!entered && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
        if (!entered) active->workflow->cancel();
        check(entered, "First panel did not start");
        auto overlap = DesignRun::start({}, "overlap-project", path + "-overlap", "Second panel", "", {}, model, search);
        wait(overlap);
        active->workflow->cancel();
        wait(active);
        check(!overlap->warning.empty() && overlap->error.empty() && overlap->assets.size() == 1,
            "Concurrent panels did not protect shared storage while retaining session results");
        auto cancel = DesignRun::start({}, "cancel-project", "", "Cancel me", "", {}, slow, search);
        cancel->workflow->cancel();
        wait(cancel);
        check(cancel->error.find("cancelled") != std::string::npos && cancel->assets.empty(), "Cancellation published results");
        AssetSearch downloadable = [search](auto const &query, auto &execution) {
            auto found = search(query, execution);
            found[0]["mime"] = "image/png";
            found[0]["download"] = "https://upload.wikimedia.org/wikipedia/commons/a/ab/Test.png";
            return found;
        };
        entered = false;
        AssetDownload slow_download = [&entered](auto const &, ExecutionContext &execution) -> std::string {
            entered = true;
            for (;;) { execution.checkpoint(); std::this_thread::sleep_for(1ms); }
        };
        auto downloading = DesignRun::start({}, "download-cancel", "", "Cancel download", "", {}, model, downloadable, slow_download);
        deadline = Clock::now() + 5s;
        while (!entered && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
        downloading->cancel();
        wait(downloading);
        check(entered && downloading->error.find("cancelled") != std::string::npos && !downloading->downloading,
            "Stopping the workflow did not interrupt its asset downloads");
        // Invalid credentials fail before any request; no secret appears in diagnostic output.
        bool rejected = false;
        try { design_model({"http", "http://example.test", "model", "key"}); } catch (...) { rejected = true; }
        check(rejected, "Insecure model endpoint accepted");
        std::filesystem::remove_all(directory);
        std::cout << "PASS: background session completion, persistence/reload, corrupt-file preservation, concurrent storage protection, cancellation and endpoint validation\n";
    } catch (std::exception const &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
