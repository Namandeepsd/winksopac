// SPDX-License-Identifier: GPL-2.0-or-later
// Two deterministic demonstration agents. No providers, network, GTK or user documents.
#include "ai/orchestrator.h"
#include "ai/svg-validation.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <glib.h>

using namespace Inkscape::AI;
using namespace std::chrono_literals;
namespace {
void check(bool condition, char const *message)
{
    if (!condition) throw std::runtime_error(message);
}
template <typename F> void rejects(F fn, char const *message)
{
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error(message);
}
TaskSpec task(std::string id, std::string capability = "tile", std::vector<std::string> dependencies = {})
{
    TaskSpec result;
    result.id = std::move(id);
    result.capability = std::move(capability);
    result.description = "Deterministic workflow demonstration";
    result.dependencies = std::move(dependencies);
    result.output_format = "image/svg+xml";
    result.timeout = 2s;
    result.retry_delay = 1ms;
    return result;
}
struct Probe {
    std::atomic<unsigned> active{0}, peak{0}, entered{0};
};
class TileAgent final : public Agent {
public:
    explicit TileAgent(std::shared_ptr<Probe> probe) : _probe(std::move(probe)) {}
    AgentDescriptor descriptor() const override
    {
        return {"tile-agent", "Test tile", "Produces a test rectangle and metadata", {"tile"},
                {"application/json"}, {"image/svg+xml", "application/json", "image/png"}, true};
    }
private:
    AgentResult execute(AgentRequest const &request, ExecutionContext &execution) override
    {
        check(status() == AgentStatus::Running, "Agent status was not running");
        check(request.model.provider == "test" && request.model.model == "tile-model", "Per-agent model configuration lost");
        check(request.context.get({Scope::Project, {}, "palette"}).is_array(), "Relevant project context not supplied");
        check(!request.context.entries.contains({Scope::Project, {}, "private-note"}), "Context permissions leaked project data");
        auto input = request.task.input;
        auto mode = input.value("mode", std::string{});
        if (mode == "unknown-exception") throw 42;
        if (mode == "transient" && request.attempt == 1) throw AgentError("api-unavailable", "Simulated transient provider failure", true);
        if (mode == "permanent") throw AgentError("api-auth", "Simulated permanent provider failure");
        if (mode == "timeout" || mode == "cancel") {
            ++_probe->entered;
            while (true) { execution.checkpoint(); std::this_thread::sleep_for(1ms); }
        }
        if (mode == "late") std::this_thread::sleep_for(25ms);
        if (mode == "parallel") {
            auto active = ++_probe->active;
            auto peak = _probe->peak.load();
            while (active > peak && !_probe->peak.compare_exchange_weak(peak, active)) {}
            ++_probe->entered;
            auto peer = input.at("peer").get<std::string>();
            execution.send(peer, MessageKind::InformationRequest, {{"question", "palette"}}, "palette-request");
            std::uint64_t last = 0;
            bool answered = false;
            while (!answered) {
                for (auto const &message : execution.receive(last, 20ms)) {
                    last = message.sequence;
                    if (message.kind == MessageKind::InformationRequest)
                        execution.send(message.from, MessageKind::Suggestion, {{"color", "#ff69b4"}}, message.correlation);
                    if (message.kind == MessageKind::Suggestion) answered = true;
                }
            }
            --_probe->active;
        }
        auto color = input.value("color", std::string("#ff69b4"));
        OutputDraft svg;
        svg.name = "tile";
        svg.format = "image/svg+xml";
        svg.kind = OutputKind::SvgComponent;
        svg.data = "<svg xmlns='http://www.w3.org/2000/svg' width='80' height='80' viewBox='0 0 80 80'><rect width='80' height='80' fill='" + color + "'/></svg>";
        svg.sources.push_back({"test://generated-tile", "Deterministic test fixture", "CC0"});
        if (mode == "malformed") svg.data = "<svg>broken";
        if (mode == "script") svg.data = "<svg xmlns='http://www.w3.org/2000/svg'><script>alert(1)</script></svg>";
        if (mode == "unregistered-format") { svg.kind = OutputKind::Image; svg.format = "image/png"; svg.data = "fake image"; }
        OutputDraft metadata;
        metadata.name = "metadata";
        metadata.format = "application/json";
        metadata.kind = OutputKind::Metadata;
        metadata.data = Json{{"color", color}}.dump();
        AgentResult result;
        result.outputs = {svg, metadata};
        result.context.push_back({{Scope::Task, request.context_task, "summary"}, "Produced a tile", 0});
        if (mode == "denied") result.context.push_back({{Scope::Project, {}, "private-note"}, "overwrite", 0});
        if (mode == "reserved") result.context.push_back({{Scope::Task, request.context_task, "outputs"}, "spoof", 0});
        if (mode == "decompose") result.tasks.push_back(task("child", "assemble"));
        if (mode == "bad-decompose") result.tasks.push_back(task(request.task.id, "assemble"));
        return result;
    }
    std::shared_ptr<Probe> _probe;
};
class AssemblyAgent final : public Agent {
public:
    AgentDescriptor descriptor() const override
    {
        return {"assembly-agent", "Test assembly", "Combines dependency tiles into one SVG", {"assemble"},
                {"application/json"}, {"image/svg+xml"}, false};
    }
private:
    AgentResult execute(AgentRequest const &request, ExecutionContext &execution) override
    {
        execution.checkpoint();
        check(request.model.model == "assembly-model", "Assembly model configuration not independent");
        check(!request.inputs.empty(), "Dependency outputs missing");
        OutputDraft composition;
        composition.name = "composition";
        composition.format = "image/svg+xml";
        composition.kind = OutputKind::Composition;
        std::string contents;
        unsigned x = 0;
        for (auto const &output : request.inputs) {
            composition.parents.push_back(output.id);
            if (output.format == "application/json") {
                auto color = Json::parse(output.data).at("color").get<std::string>();
                contents += "<rect x='" + std::to_string(x) + "' width='80' height='80' fill='" + color + "'/>";
                x += 80;
            }
        }
        check(x > 0, "No metadata to compose");
        composition.data = "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(x) + "' height='80' viewBox='0 0 " + std::to_string(x) + " 80'>" + contents + "</svg>";
        return {{composition}, {}, {}};
    }
};
struct Fixture {
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    std::shared_ptr<AgentRegistry> registry = std::make_shared<AgentRegistry>();
    std::shared_ptr<ContextManager> context = std::make_shared<ContextManager>("test-project");
    std::shared_ptr<OutputRegistry> outputs = std::make_shared<OutputRegistry>();
    Fixture()
    {
        context->set_project("palette", Json::array({"#ff69b4", "#3366ff"}));
        context->set_project("canvas", {{"width", 160}, {"height", 80}, {"unit", "px"}});
        context->set_project("private-note", "Host-only fixture");
        registry->add([probe = probe] { return std::make_unique<TileAgent>(probe); },
                      {"test", "tile-model", "", Json::object()}, {{"palette", "canvas"}, {}});
        registry->add([] { return std::make_unique<AssemblyAgent>(); },
                      {"another-test-provider", "assembly-model", "", Json::object()}, {{"canvas"}, {}});
    }
    WorkflowSnapshot run(std::vector<TaskSpec> plan, unsigned concurrency = 4)
    {
        Orchestrator orchestrator(registry, context, outputs, concurrency);
        return orchestrator.start("Build a test composition", std::move(plan)).get();
    }
};
void context_tests(std::filesystem::path const &directory)
{
    ContextManager context("project-a");
    context.set_project("prompt", "test");
    context.commit("alpha", "run/task", {}, {{{Scope::Task, "run/task", "value"}, 1, 0}});
    auto old = context.snapshot();
    rejects([&] { context.commit("alpha", "run/task", {}, {
        {{Scope::Task, "run/task", "new-value"}, 2, 0}, {{Scope::Project, {}, "prompt"}, "bad", 0}}); }, "Denied context update accepted");
    check(context.snapshot().entries.size() == old.entries.size(), "Context transaction partially committed");
    rejects([&] { context.commit("alpha", "run/task", {}, {{{Scope::Task, "run/task", "value"}, 2, 0}}); }, "Stale revision accepted");
    auto revision = old.entries.at({Scope::Task, "run/task", "value"}).revision;
    rejects([&] { context.commit("beta", "run/task", {}, {{{Scope::Task, "run/task", "value"}, 3, revision}}); }, "Another agent overwrote context");
    context.commit("alpha", "run/task", {}, {{{Scope::Task, "run/task", "value"}, 2, revision}});
    auto path = (directory / "context.json").string();
    context.save(path);
    ContextManager restored("project-a");
    restored.load(path);
    check(restored.snapshot().get({Scope::Task, "run/task", "value"}) == 2, "Context did not survive reload");
    ContextManager other("project-b");
    rejects([&] { other.load(path); }, "Cross-project context accepted");
    std::ofstream(path) << "{broken";
    rejects([&] { restored.load(path); }, "Malformed context accepted");
    check(restored.snapshot().get({Scope::Task, "run/task", "value"}) == 2, "Failed context load corrupted memory");
}
void graph_and_failure_tests()
{
    for (auto mode : {"malformed", "script", "denied", "reserved", "unregistered-format", "permanent", "unknown-exception", "bad-decompose"}) {
        Fixture f;
        auto source = task("source"); source.input["mode"] = mode;
        source.max_attempts = 3;
        auto result = f.run({source, task("dependent", "assemble", {"source"}), task("independent")});
        check(result.state == WorkflowState::Failed, "Invalid workflow succeeded");
        check(result.tasks.at("source").attempts == 1, "Permanent/validation failure was retried");
        check(result.tasks.at("dependent").error_code == "dependency", "Dependency failure not propagated");
        check(result.tasks.at("independent").state == TaskState::Completed, "Independent branch did not complete");
        check(f.outputs->list().size() == 2, "Failed result partially published outputs");
        check(!f.context->snapshot().entries.contains({Scope::Task, result.id + "/source", "summary"}), "Failed result partially published context");
    }
    for (auto plan : {std::vector{task("a", "tile", {"b"}), task("b", "tile", {"a"})},
                      std::vector{task("a", "tile", {"missing"})}, std::vector{task("a"), task("a")},
                      std::vector{task("a", "unknown")}, std::vector{task("a", "tile", {"a"})}}) {
        Fixture f;
        auto result = f.run(plan);
        check(result.state == WorkflowState::Failed && !result.error.empty(), "Bad task graph accepted");
        check(f.outputs->list().empty(), "Bad graph executed tasks");
        check(!f.context->snapshot().entries.contains({Scope::Project, {}, "original_prompt"}), "Bad graph mutated prompt");
    }
    Fixture f;
    auto retry = task("retry"); retry.input["mode"] = "transient"; retry.max_attempts = 2;
    auto result = f.run({retry});
    check(result.state == WorkflowState::Completed && result.tasks.at("retry").attempts == 2, "Retry failed");
    check(f.outputs->list().size() == 2, "Retry duplicated artifacts");
    auto repeated = f.run({task("retry")});
    check(repeated.state == WorkflowState::Completed, "Task namespace collided across workflows");
    auto expand = task("plan"); expand.input["mode"] = "decompose";
    auto expanded = f.run({expand});
    check(expanded.state == WorkflowState::Completed && expanded.tasks.size() == 2, "Dynamic decomposition failed");
    check(expanded.tasks.at("child").spec.dependencies == std::vector<std::string>{"plan"}, "Child missing implicit dependency");
}
void cancellation_tests()
{
    for (auto mode : {"timeout", "late"}) {
        Fixture f;
        auto source = task("slow"); source.input["mode"] = mode; source.timeout = 8ms; source.max_attempts = 2;
        auto result = f.run({source});
        check(result.tasks.at("slow").error_code == "timeout", "Timeout not surfaced");
        check(f.outputs->list().empty(), "Timed-out output published");
    }
    Fixture f;
    auto source = task("cancel"); source.input["mode"] = "cancel";
    Orchestrator orchestrator(f.registry, f.context, f.outputs, 1);
    auto future = orchestrator.start("Cancel demo", {source, task("waiting", "assemble", {"cancel"})});
    auto until = Clock::now() + 1s;
    while (!f.probe->entered && Clock::now() < until) std::this_thread::sleep_for(1ms);
    check(f.probe->entered > 0, "Cancellation task did not start");
    // Same project must not admit a second concurrent workflow.
    auto competing = f.run({task("other")});
    check(competing.state == WorkflowState::Failed && competing.error.find("active workflow") != std::string::npos, "Concurrent project ownership allowed");
    orchestrator.cancel();
    auto result = future.get();
    check(result.state == WorkflowState::Cancelled, "Workflow cancellation not recorded");
    check(result.tasks.at("waiting").error_code == "cancelled", "Pending work survived cancellation");
    check(f.outputs->list().empty(), "Cancelled task published output");
    check(f.run({task("next")}).state == WorkflowState::Completed, "Project lease was not released");
}
void message_tests()
{
    auto bus = std::make_shared<MessageBus>();
    bus->register_tasks({"one", "two"});
    std::stop_source stop;
    ExecutionContext one("one", 1, bus, stop.get_token(), Clock::now() + 1s);
    ExecutionContext two("two", 1, bus, stop.get_token(), Clock::now() + 1s);
    rejects([&] { one.send("unknown", MessageKind::Suggestion, Json::object()); }, "Unknown message recipient accepted");
    rejects([&] { one.send("two", MessageKind::Completed, Json::object()); }, "Agent forged completion");
    rejects([&] { one.send("two", MessageKind::Suggestion, std::string(9000, 'x')); }, "Oversized message accepted");
    one.send("two", MessageKind::ContextRequest, {{"field", "style"}});
    check(one.receive().empty(), "Agent read another task's inbox");
    auto messages = two.receive();
    check(messages.size() == 1 && messages.front().from == "one", "Message sender identity lost");
    check(two.receive(messages.back().sequence).empty(), "Message cursor repeated earlier messages");
    for (int i = 1; i < 64; ++i) one.send("two", MessageKind::Suggestion, {{"n", i}});
    rejects([&] { one.send("two", MessageKind::Suggestion, Json::object()); }, "Agent message budget not enforced");
    auto waiting = std::async(std::launch::async, [&] { rejects([&] { two.receive(1000, 1s); }, "Cancelled message wait returned normally"); });
    stop.request_stop();
    check(waiting.wait_for(200ms) == std::future_status::ready, "Message wait ignored cancellation");
    waiting.get();
}
void demo(std::filesystem::path const &directory)
{
    Fixture f;
    rejects([&] { f.registry->add([probe = f.probe] { return std::make_unique<TileAgent>(probe); }); }, "Duplicate agent ID accepted");
    check(f.registry->discover("tile").size() == 1, "Capability discovery failed");
    Orchestrator orchestrator(f.registry, f.context, f.outputs, 2);
    auto future = orchestrator.start("Compose pink and blue tiles", [](auto const &prompt, auto const &context) {
        check(!prompt.empty() && context.project == "test-project", "Decomposer inputs missing");
        auto pink = task("pink"), blue = task("blue");
        pink.input = {{"mode", "parallel"}, {"peer", "blue"}, {"color", "#ff69b4"}};
        blue.input = {{"mode", "parallel"}, {"peer", "pink"}, {"color", "#3366ff"}};
        return std::vector{pink, blue, task("compose", "assemble", {"pink", "blue"})};
    });
    auto result = future.get();
    if (result.state != WorkflowState::Completed) {
        for (auto const &[id, record] : result.tasks) std::cerr << id << ": " << record.error << '\n';
    }
    check(result.state == WorkflowState::Completed, "Demo workflow failed");
    check(result.progress() == 1 && f.probe->peak == 2, "Independent tasks did not execute concurrently");
    check(result.tasks.at("compose").spec.agent == "assembly-agent", "Dynamic assignment failed");
    check(orchestrator.messages().size() >= 7, "Structured inter-agent communication missing");
    check(f.outputs->list().size() == 5, "Output collection incomplete");
    auto final_id = result.tasks.at("compose").outputs.front();
    auto composition = f.outputs->get(final_id);
    check(composition.parents.size() == 4 && composition.agent == "assembly-agent", "Output provenance missing");
    std::ofstream(directory / "composition.svg") << f.outputs->preview_svg(final_id);
    auto archive = (directory / "outputs.json").string();
    f.outputs->save(archive, "test-project");
    OutputRegistry restored;
    restored.load(archive, "test-project");
    check(restored.preview_svg(final_id) == f.outputs->preview_svg(final_id), "Output archive failed round trip");
    check(restored.get(result.tasks.at("pink").outputs.front()).sources.front().license == "CC0", "Asset provenance lost");
    rejects([&] { restored.load(archive, "test-project"); }, "Live output registry overwritten");
    OutputRegistry other;
    rejects([&] { other.load(archive, "wrong-project"); }, "Cross-project output archive accepted");
    Json corrupted;
    { std::ifstream input(archive); input >> corrupted; }
    corrupted["outputs"][0]["parents"] = Json::array({corrupted["outputs"][0]["id"]});
    std::ofstream(directory / "bad-output.json") << corrupted;
    rejects([&] { other.load((directory / "bad-output.json").string(), "test-project"); }, "Circular archived provenance accepted");
    check(other.list().empty(), "Invalid archive partially loaded");
    f.context->save((directory / "demo-context.json").string());
    ContextManager loaded("test-project");
    loaded.load((directory / "demo-context.json").string());
    check(loaded.snapshot().get({Scope::Project, {}, "iterations"}).size() == 1, "Design iteration history missing");
    std::uint64_t previous = 0;
    for (auto const &event : result.events) { check(event.sequence > previous, "Event ordering is not stable"); previous = event.sequence; }
    rejects([&] { orchestrator.start("second start", {task("no")}); }, "Workflow reused accidentally");
    std::cout << "Demo: 2 registered agents, 2 parallel tile tasks, 1 dependent composition, 5 validated outputs\n";
}
}
int main(int argc, char **argv)
{
    g_autofree char *temporary = g_dir_make_tmp("inkscape-workflow-test-XXXXXX", nullptr);
    if (!temporary) return 1;
    auto directory = std::filesystem::path(temporary);
    try {
        context_tests(directory);
        graph_and_failure_tests();
        cancellation_tests();
        message_tests();
        demo(directory);
        if (argc > 1) std::filesystem::copy_file(directory / "composition.svg", argv[1], std::filesystem::copy_options::overwrite_existing);
        std::cout << "PASS: scheduling, scoped context, atomic publication, messages, retries, cancellation, timeouts, DAG validation, dynamic decomposition, provenance, persistence and SVG validation\n";
        std::filesystem::remove_all(directory);
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "FAIL: " << error.what() << " (fixtures: " << directory << ")\n";
        return 1;
    }
}
