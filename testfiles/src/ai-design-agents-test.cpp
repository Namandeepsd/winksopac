// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai/design-agents.h"
#include "ai/orchestrator.h"
#include <iostream>
#include <atomic>
#include <thread>
using namespace Inkscape::AI;
namespace {
void check(bool condition, char const *reason) { if (!condition) throw std::runtime_error(reason); }
Json plan(unsigned queries = 2)
{
    Json result = {{"title", "Solar energy poster"}, {"summary", "Explain energy conversion"}, {"style", "Clean scientific diagram"},
        {"palette", Json::array({"#123456", "#abcdef"})}, {"layout", Json::array({{{"section", "Overview"}, {"content", "Use supplied data; otherwise a placeholder"}}})}, {"asset_queries", Json::array()}};
    for (unsigned i = 0; i < queries; ++i) result["asset_queries"].push_back({{"query", "solar cell " + std::to_string(i)}, {"purpose", "Find a reference diagram"}});
    return result;
}
Json candidates()
{
    return commons_assets(Json::parse(R"JSON({"query":{"pages":[{"title":"File:Solar.svg","imageinfo":[{"descriptionurl":"https://commons.wikimedia.org/wiki/File:Solar.svg","mime":"image/svg+xml","extmetadata":{"Artist":{"value":"<b>A &amp; B</b>"},"LicenseShortName":{"value":"CC BY-SA 4.0"},"ImageDescription":{"value":"<p>Solar diagram</p>"}}}]}]}})JSON"));
}
WorkflowSnapshot run(Json raw_plan, Json ranking, Json found, std::shared_ptr<OutputRegistry> outputs,
    std::atomic<unsigned> &calls, bool fail_search = false)
{
    auto context = std::make_shared<ContextManager>("test");
    context->set_project("canvas", {{"width", 800}, {"height", 600}});
    context->set_project("style", "Editorial");
    auto registry = std::make_shared<AgentRegistry>();
    DesignModel model = [=](auto const &, auto const &, Json const &input, ExecutionContext &execution) {
        execution.checkpoint();
        if (input.contains("request")) return "```json\n" + raw_plan.dump() + "\n```";
        return ranking.dump();
    };
    AssetSearch search = [&, found, fail_search](auto const &, ExecutionContext &execution) {
        ++calls; execution.checkpoint();
        if (fail_search) throw AgentError("network", "Simulated service failure");
        return found;
    };
    registry->add([model] { return std::make_unique<PlannerAgent>(model); }, {}, {{"style", "canvas"}, {}});
    registry->add([model, search] { return std::make_unique<AssetDiscoveryAgent>(model, search); });
    Orchestrator workflow(registry, context, outputs, 3);
    TaskSpec root; root.id = "plan"; root.agent = "planner"; root.description = "Plan a poster"; root.input = {{"prompt", "Design a solar poster"}};
    return workflow.start("Design a solar poster", std::vector{root}).get();
}
}
int main()
{
    try {
        auto found = candidates();
        check(found.size() == 1 && found[0]["attribution"] == "A & B", "Commons HTML metadata was not converted to text");
        for (auto const &url : {"https://commons.wikimedia.org.evil/wiki/File:X", "http://commons.wikimedia.org/wiki/File:X", "https://user@commons.wikimedia.org/wiki/File:X", "https://commons.wikimedia.org:444/wiki/File:X", "javascript:alert(1)"})
            check(!commons_page(url), "Unsafe asset URL accepted");
        std::atomic<unsigned> calls{0};
        auto outputs = std::make_shared<OutputRegistry>();
        auto result = run(plan(), {{"indices", Json::array({0})}}, found, outputs, calls);
        check(result.state == WorkflowState::Completed && result.tasks.size() == 3 && calls == 2, "Planner did not dispatch real discovery agents");
        auto items = outputs->list();
        check(items.size() == 3, "Plan and discovery results missing");
        for (auto const &item : items) if (item.agent == "asset-discovery") {
            check(item.sources.size() == 1 && item.sources[0].license == "CC BY-SA 4.0", "Source license lost");
            check(item.parents.size() == 1, "Discovery provenance missing plan dependency");
        }
        for (auto const &item : items) if (item.agent == "planner") {
            auto planned = Json::parse(item.data);
            check(planned.at("layout_search").at("status") == "solved", "Layout search not integrated");
            check(planned.at("layout")[0].contains("box_percent"), "Solved section bounds missing");
        }
        auto large = plan(0);
        for (int i = 1; i < 8; ++i) large["layout"].push_back(large["layout"][0]);
        auto large_outputs = std::make_shared<OutputRegistry>();
        check(run(large, {}, found, large_outputs, calls).state == WorkflowState::Completed, "Eight-section layout failed");
        auto solved = Json::parse(large_outputs->list().at(0).data);
        auto const &sections = solved.at("layout");
        for (std::size_t i = 0; i < sections.size(); ++i) {
            auto a = sections[i].at("box_percent");
            int x = a["x"], y = a["y"], w = a["width"], h = a["height"];
            check(x >= 5 && y >= 5 && x + w <= 95 && y + h <= 95, "Section outside margins");
            for (std::size_t j = 0; j < i; ++j) {
                auto b = sections[j].at("box_percent");
                check(x + w + 3 <= b["x"].get<int>() || b["x"].get<int>() + b["width"].get<int>() + 3 <= x ||
                      y + h + 3 <= b["y"].get<int>() || b["y"].get<int>() + b["height"].get<int>() + 3 <= y,
                      "Sections violate separation");
            }
        }
        calls = 0;
        check(run(plan(0), {}, found, std::make_shared<OutputRegistry>(), calls).state == WorkflowState::Completed && calls == 0, "No-query plan triggered a search");
        check(run(plan(1), {}, Json::array(), std::make_shared<OutputRegistry>(), calls).state == WorkflowState::Completed, "Empty search result failed workflow");
        for (auto ranking : {Json{{"indices", Json::array({99})}}, Json{{"indices", Json::array({0, 0})}}, Json{{"indices", Json::array({-1})}}, Json{{"indices", Json::array({4294967296ULL})}}, Json{{"indices", "all"}}}) {
            auto store = std::make_shared<OutputRegistry>();
            auto failed = run(plan(1), ranking, found, store, calls);
            check(failed.state == WorkflowState::Failed && store->list().size() == 1, "Invented/invalid asset selection published");
        }
        for (auto broken : {plan(4), Json{{"title", "incomplete"}}}) {
            calls = 0;
            auto failed = run(broken, {}, found, std::make_shared<OutputRegistry>(), calls);
            check(failed.state == WorkflowState::Failed && calls == 0, "Malformed plan launched discovery");
        }
        auto network = run(plan(1), {}, found, std::make_shared<OutputRegistry>(), calls, true);
        check(network.tasks.at("plan").state == TaskState::Completed && network.tasks.at("assets-1").state == TaskState::Failed, "Search failure destroyed usable plan");
        auto prompt = design_preview_prompt("solar", plan(), found);
        check(prompt.find("not downloaded images") != std::string::npos && prompt.find("CC BY-SA") != std::string::npos, "Preview handoff lost reference constraints");
        std::cout << "PASS: Planner decomposition, discovery ranking, source provenance, Commons metadata, URL validation, empty results, malformed responses and partial failure\n";
    } catch (std::exception const &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
