// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-design-services.h"
#include "ai-codex.h"
#include "async/async.h"
#include <libsoup/soup.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <filesystem>
#include <algorithm>
#include <stop_token>
#include <mutex>
#include <map>
namespace Inkscape::UI::Dialog {
using namespace Inkscape::AI;
namespace {
// Two panels can target the same saved document. Never let their independent
// context snapshots replace each other's archive; the second run stays ephemeral.
std::shared_ptr<std::mutex> storage_mutex(std::string const &path)
{
    static std::mutex guard;
    static std::map<std::string, std::weak_ptr<std::mutex>> locks;
    std::lock_guard lock(guard);
    std::erase_if(locks, [](auto const &entry) { return entry.second.expired(); });
    auto &entry = locks[path];
    auto result = entry.lock();
    if (!result) { result = std::make_shared<std::mutex>(); entry = result; }
    return result;
}
void require_endpoint(std::string const &url)
{
    g_autoptr(GUri) uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
    if (!uri || g_strcmp0(g_uri_get_scheme(uri), "https") || !g_uri_get_host(uri) || g_uri_get_userinfo(uri))
        throw AgentError("connection", "Enter a valid HTTPS chat-completions URL in Connection settings.");
}
std::string http(std::string const &url, std::string const &body, std::string const &key, ExecutionContext &execution, std::size_t limit = 1024 * 1024)
{
    require_endpoint(url);
    execution.checkpoint();
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    std::stop_callback stopped(execution.stop_token(), [cancel] { g_cancellable_cancel(cancel); });
    g_autoptr(SoupSession) session = soup_session_new();
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(execution.deadline() - Clock::now()).count();
    soup_session_set_timeout(session, std::clamp<decltype(remaining)>(remaining, 1, 45));
    soup_session_set_user_agent(session, "WinkSpace-Inkscape/1.0 (design asset discovery; https://inkscape.org)");
    g_autoptr(SoupMessage) message = soup_message_new(body.empty() ? "GET" : "POST", url.c_str());
    if (!message) throw AgentError("connection", "Cannot create HTTP request");
    soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    if (!key.empty()) {
        if (key.find_first_of("\r\n") != std::string::npos) throw AgentError("connection", "API key contains a line break");
        soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", ("Bearer " + key).c_str());
    }
    if (!body.empty()) {
        g_autoptr(GBytes) bytes = g_bytes_new(body.data(), body.size());
        soup_message_set_request_body_from_bytes(message, "application/json", bytes);
    }
    g_autoptr(GError) error = nullptr;
    g_autoptr(GInputStream) stream = soup_session_send(session, message, cancel, &error);
    if (!stream) {
        execution.checkpoint();
        throw AgentError("network", "Connection failed. Check your network and provider settings.", true);
    }
    auto status = soup_message_get_status(message);
    if (status < 200 || status >= 300) throw AgentError("http", "HTTP " + std::to_string(status) +
        (body.empty() ? " from Wikimedia Commons." : " from the model provider. Check connection settings."), status == 429 || status >= 500);
    std::string result;
    char buffer[8192];
    while (true) {
        execution.checkpoint();
        auto count = g_input_stream_read(stream, buffer, sizeof(buffer), cancel, &error);
        if (count < 0) { execution.checkpoint(); throw AgentError("network", "Response interrupted", true); }
        if (!count) break;
        if (result.size() + count > limit) throw AgentError("response-size", "Download exceeded the size limit");
        result.append(buffer, count);
    }
    return result;
}
}
DesignModel design_model(DesignConnection connection)
{
    validate_connection(connection);
    return [connection = std::move(connection)](ModelConfig const &model, std::string const &system, Json const &input, ExecutionContext &execution) {
        execution.checkpoint();
        Json body = {{"model", model.model}, {"stream", false}, {"max_completion_tokens", 6000},
            {"messages", Json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", input.dump()}}})}};
        auto selected = connection;
        if (!model.model.empty()) selected.model = model.model;
        body["model"] = selected.model;
        g_autoptr(GCancellable) cancel = g_cancellable_new();
        std::stop_callback stopped(execution.stop_token(), [cancel] { g_cancellable_cancel(cancel); });
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(execution.deadline() - Clock::now()).count();
        auto response = run_connection(selected, body.dump(), cancel, std::clamp<decltype(remaining)>(remaining, 1, 1800));
        execution.checkpoint();
        auto envelope = parse_design_json(response);
        auto const &choice = envelope.at("choices").at(0);
        if (choice.value("finish_reason", std::string{}) == "length") throw AgentError("length", "The model response was truncated. Try a shorter brief or another model.");
        if (!choice.at("message").at("content").is_string()) throw AgentError("response", "The model returned no text response");
        return choice["message"]["content"].get<std::string>();
    };
}
Json search_commons(std::string const &query, ExecutionContext &execution)
{
    g_autofree char *escaped = g_uri_escape_string(query.c_str(), nullptr, false);
    std::string url = "https://commons.wikimedia.org/w/api.php?action=query&format=json&formatversion=2&generator=search&gsrnamespace=6&gsrlimit=6&gsrsearch=";
    url += escaped;
    url += "&prop=imageinfo&iiprop=url%7Cmime%7Cextmetadata&iiurlwidth=800&iiextmetadatafilter=LicenseShortName%7CUsageTerms%7CArtist%7CCredit%7CImageDescription&iiextmetadatalanguage=en";
    return commons_assets(parse_design_json(http(url, {}, {}, execution)));
}
std::string download_commons_asset(std::string const &url, ExecutionContext &execution)
{
    if (!commons_download_url(url)) throw AgentError("asset-url", "Unsupported asset download host");
    return http(url.substr(0, url.find('?')), {}, {}, execution, 4 * 1024 * 1024);
}
std::shared_ptr<DesignRun> DesignRun::start(DesignConnection connection, std::string project, std::string storage,
    std::string prompt, std::string brief, Json canvas, DesignModel model, AssetSearch search, AssetDownload download)
{
    if (prompt.empty() || prompt.size() > 16000 || brief.size() > 8000) throw AgentError("input", "Write a design request (up to 16,000 bytes) and keep the brief under 8,000 bytes.");
    if (!model) model = design_model(connection);
    if (!search) search = search_commons;
    if (!download) download = download_commons_asset;
    auto registry = std::make_shared<AgentRegistry>();
    ModelConfig config{connection.provider, connection.model, "sidebar", Json::object()};
    registry->add([model] { return std::make_unique<PlannerAgent>(model); }, config, {{"style", "canvas"}, {}});
    registry->add([model, search] { return std::make_unique<AssetDiscoveryAgent>(model, search); }, config);
    auto run = std::make_shared<DesignRun>();
    auto context = std::make_shared<ContextManager>(project);
    run->outputs = std::make_shared<OutputRegistry>();
    run->workflow = std::make_shared<Orchestrator>(registry, context, run->outputs, 3);
    run->prompt = prompt;
    Async::fire_and_forget([run, context, download, storage = std::move(storage), project, prompt = std::move(prompt), brief = std::move(brief), canvas = std::move(canvas)] {
        bool writable = true;
        try {
            auto mutex = storage_mutex(storage);
            std::unique_lock storage_lock(*mutex, std::defer_lock);
            if (!storage.empty() && !storage_lock.try_lock()) {
                writable = false;
                run->warning = "Another workflow is using this project's storage; results remain available for this session.";
            }
            if (writable && !storage.empty()) {
                try {
                    if (std::filesystem::exists(storage + ".outputs.json")) run->outputs->load(storage + ".outputs.json", project);
                    if (std::filesystem::exists(storage + ".context.json")) context->load(storage + ".context.json");
                } catch (std::exception const &) { writable = false; run->warning = "Previous workflow storage could not be loaded; its files will be preserved."; }
            }
            context->set_project("style", brief);
            context->set_project("canvas", canvas);
            TaskSpec task;
            task.id = "plan"; task.agent = "planner"; task.capability = "plan-design";
            task.description = "Plan the design and identify reference needs";
            task.input = {{"prompt", prompt}}; task.max_attempts = 2; task.timeout = std::chrono::seconds(180);
            auto report = run->workflow->start(prompt, std::vector<TaskSpec>{task}).get();
            for (auto const &output : run->outputs->list(report.id)) {
                auto data = Json::parse(output.data);
                if (output.agent == "planner") run->plan = data;
                if (output.agent == "asset-discovery") for (auto const &asset : data.at("assets")) {
                    if (std::none_of(run->assets.begin(), run->assets.end(), [&](auto const &existing) { return existing.at("source") == asset.at("source"); }))
                        run->assets.push_back(asset);
                }
            }
            if (report.state == WorkflowState::Cancelled) run->error = "Design workflow cancelled.";
            else if (report.state == WorkflowState::Failed) {
                run->error = report.error;
                for (auto const &[id, task] : report.tasks) if (task.state == TaskState::Failed) {
                    if (!run->error.empty()) run->error += "\n";
                    run->error += task.spec.description + ": " + task.error;
                }
            }
            if (report.state != WorkflowState::Cancelled && !run->download_stop.stop_requested()) {
                run->downloading = true;
                auto bus = std::make_shared<MessageBus>();
                bus->register_tasks({"download"});
                ExecutionContext execution("download", 1, bus, run->download_stop.get_token(), Clock::now() + std::chrono::seconds(90));
                for (auto const &asset : run->assets) {
                    if (run->download_stop.stop_requested()) break;
                    run->downloaded.push_back(prepare_design_asset(asset, "asset-" + std::to_string(run->downloaded.size() + 1), download, execution));
                }
                run->downloading = false;
            }
            if (writable && !storage.empty()) {
                try {
                    auto directory = std::filesystem::path(storage).parent_path().string();
                    if (g_mkdir_with_parents(directory.c_str(), 0700) != 0) throw std::runtime_error("Cannot create workflow storage");
                    run->outputs->save(storage + ".outputs.json", project);
                    context->save(storage + ".context.json");
                } catch (std::exception const &) { run->warning = "Workflow results could not be saved; this session's results remain available."; }
            }
        } catch (std::exception const &error) { run->error = error.what(); }
        if (run->download_stop.stop_requested()) run->error = "Design workflow cancelled.";
        run->downloading = false;
        run->done.store(true); // Publishes plan/assets/error/warning to the GTK polling thread.
    });
    return run;
}
} // namespace Inkscape::UI::Dialog
