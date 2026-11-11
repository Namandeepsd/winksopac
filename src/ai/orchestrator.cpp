// SPDX-License-Identifier: GPL-2.0-or-later
#include "orchestrator.h"
#include <algorithm>
#include <thread>
#include <glib.h>

namespace Inkscape::AI {
namespace {
constexpr std::size_t max_tasks = 128;
bool terminal(TaskState state) { return state == TaskState::Completed || state == TaskState::Failed; }
using Bindings = std::map<std::string, AgentBinding>;

void validate_graph(std::map<std::string, TaskRecord> const &tasks, Bindings const &agents)
{
    if (tasks.empty() || tasks.size() > max_tasks) throw std::invalid_argument("Workflow requires 1–128 tasks");
    for (auto const &[id, record] : tasks) {
        auto const &task = record.spec;
        require_id(id);
        if (id == "host") throw std::invalid_argument("Reserved task ID");
        require_json(task.input);
        if (task.description.empty() || task.description.size() > 16000 || !task.max_attempts || task.max_attempts > 5 ||
            task.timeout < std::chrono::milliseconds(1) || task.timeout > std::chrono::minutes(30) ||
            task.retry_delay < std::chrono::milliseconds(0) || task.retry_delay > std::chrono::minutes(1))
            throw std::invalid_argument("Invalid task limits or description: " + id);
        auto compatible = [&](auto const &entry) {
            auto const &d = entry.second.descriptor;
            return (task.agent.empty() || task.agent == d.id) &&
                   (task.capability.empty() || d.capabilities.contains(task.capability)) &&
                   d.input_formats.contains(task.input_format) && d.output_formats.contains(task.output_format);
        };
        if ((task.agent.empty() && task.capability.empty()) || std::none_of(agents.begin(), agents.end(), compatible))
            throw std::invalid_argument("No compatible agent for task: " + id);
        std::set<std::string> unique;
        for (auto const &dependency : task.dependencies) {
            if (!tasks.contains(dependency) || !unique.insert(dependency).second)
                throw std::invalid_argument("Missing or repeated dependency: " + dependency);
        }
    }
    std::map<std::string, int> visited;
    std::function<void(std::string const &)> visit = [&](std::string const &id) {
        if (visited[id] == 1) throw std::invalid_argument("Circular task dependency at: " + id);
        if (visited[id] == 2) return;
        visited[id] = 1;
        for (auto const &dep : tasks.at(id).spec.dependencies) visit(dep);
        visited[id] = 2;
    };
    for (auto const &[id, record] : tasks) visit(id);
}
std::string assign(TaskSpec const &task, Bindings const &agents, std::map<std::string, unsigned> const &load)
{
    std::string chosen;
    unsigned least = ~0u;
    for (auto const &[id, binding] : agents) {
        auto const &d = binding.descriptor;
        if ((!task.agent.empty() && task.agent != id) ||
            (!task.capability.empty() && !d.capabilities.contains(task.capability)) ||
            !d.input_formats.contains(task.input_format) || !d.output_formats.contains(task.output_format)) continue;
        auto busy = load.contains(id) ? load.at(id) : 0;
        if (busy < least) { least = busy; chosen = id; }
    }
    if (chosen.empty()) throw std::runtime_error("No agent available");
    return chosen;
}
struct Attempt {
    std::future<AgentResult> future;
    std::stop_source stop;
    Clock::time_point deadline;
};
}
struct Orchestrator::State {
    mutable std::mutex mutex;
    WorkflowSnapshot view;
    std::shared_ptr<AgentRegistry> registry;
    std::shared_ptr<ContextManager> context;
    std::shared_ptr<OutputRegistry> outputs;
    std::shared_ptr<MessageBus> bus = std::make_shared<MessageBus>();
    std::stop_source stop;
    unsigned concurrency;
    bool started = false;
    std::uint64_t sequence = 0;
    void event(std::string kind, TaskRecord const *task = nullptr, std::string detail = {})
    {
        std::lock_guard lock(mutex);
        if (task) view.tasks[task->spec.id] = *task;
        if (view.events.size() == 4096) view.events.erase(view.events.begin());
        view.events.push_back({++sequence, std::move(kind), task ? task->spec.id : "", task ? task->spec.agent : "",
                               std::move(detail), task ? task->state : TaskState::Pending});
    }
    WorkflowSnapshot snapshot() const { std::lock_guard lock(mutex); return view; }
};
Orchestrator::Orchestrator(std::shared_ptr<AgentRegistry> registry, std::shared_ptr<ContextManager> context,
                           std::shared_ptr<OutputRegistry> outputs, unsigned concurrency)
    : _state(std::make_shared<State>())
{
    if (!registry || !context || !outputs || concurrency < 1 || concurrency > 16)
        throw std::invalid_argument("Invalid workflow services or concurrency");
    _state->registry = std::move(registry);
    _state->context = std::move(context);
    _state->outputs = std::move(outputs);
    _state->concurrency = concurrency;
    g_autofree char *uuid = g_uuid_string_random();
    _state->view.id = uuid;
}
Orchestrator::~Orchestrator() { cancel(); }
void Orchestrator::cancel() { _state->stop.request_stop(); }
WorkflowSnapshot Orchestrator::snapshot() const { return _state->snapshot(); }
std::vector<Message> Orchestrator::messages() const { return _state->bus->snapshot(); }
std::future<WorkflowSnapshot> Orchestrator::start(std::string prompt, TaskDecomposer decompose)
{
    if (!decompose || prompt.empty() || prompt.size() > 16000) throw std::invalid_argument("Prompt and decomposition policy required");
    std::lock_guard lock(_state->mutex);
    if (_state->started) throw std::logic_error("An orchestrator can only run once");
    _state->started = true;
    return std::async(std::launch::async, &Orchestrator::run, _state, std::move(prompt), std::move(decompose));
}
std::future<WorkflowSnapshot> Orchestrator::start(std::string prompt, std::vector<TaskSpec> tasks)
{
    return start(std::move(prompt), [tasks = std::move(tasks)](auto const &, auto const &) { return tasks; });
}
WorkflowSnapshot Orchestrator::run(std::shared_ptr<State> state, std::string prompt, TaskDecomposer decompose)
{
    std::map<std::string, TaskRecord> tasks;
    std::map<std::string, Attempt> active;
    std::map<std::string, Clock::time_point> ready_at;
    auto agents = state->registry->snapshot();
    auto id = state->snapshot().id;
    bool context_acquired = false;
    bool plan_accepted = false;
    auto fail = [&](TaskRecord &task, std::string code, std::string error) {
        task.state = TaskState::Failed;
        task.error_code = std::move(code);
        task.error = error.substr(0, 4096);
        state->event("task-failed", &task, task.error);
        // Terminal events are always retained even if an agent exhausted messaging quota.
        try { state->bus->send(task.spec.id, "host", MessageKind::Failed, {{"code", task.error_code}, {"error", task.error}}, {}); }
        catch (std::exception const &) {}
    };
    try {
        state->context->begin_workflow(id);
        context_acquired = true;
        std::vector<TaskSpec> plan;
        try { plan = decompose(prompt, state->context->snapshot()); }
        catch (std::exception const &) { throw; }
        catch (...) { throw std::runtime_error("Decomposition policy threw an unknown exception"); }
        for (auto const &task : plan) {
            if (!tasks.emplace(task.id, TaskRecord{task}).second) throw std::invalid_argument("Duplicate task ID: " + task.id);
        }
        validate_graph(tasks, agents); // No execution or context mutation for invalid plans.
        plan_accepted = true;
        std::vector<std::string> endpoints;
        for (auto const &[key, task] : tasks) endpoints.push_back(key);
        state->bus->register_tasks(endpoints);
        state->context->set_project("original_prompt", prompt);
        {
            std::lock_guard lock(state->mutex);
            state->view.tasks = tasks;
            state->view.state = WorkflowState::Running;
        }
        state->event("workflow-started");
        while (true) {
            for (auto it = active.begin(); it != active.end();) {
                auto &attempt = it->second;
                auto &task = tasks.at(it->first);
                bool expired = Clock::now() >= attempt.deadline;
                if (expired || state->stop.stop_requested()) attempt.stop.request_stop();
                if (attempt.future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { ++it; continue; }
                try {
                    auto result = attempt.future.get();
                    if (expired) throw AgentError("timeout", "Task deadline exceeded", true);
                    if (state->stop.stop_requested()) throw AgentError("cancelled", "Workflow cancelled");
                    auto const &binding = agents.at(task.spec.agent);
                    if (!result.tasks.empty() && !binding.descriptor.can_decompose)
                        throw AgentError("decomposition", "Agent cannot add tasks");
                    auto expanded = tasks;
                    std::vector<std::string> new_ids;
                    for (auto child : result.tasks) {
                        // Child work always waits for its decomposer's validated result.
                        if (std::find(child.dependencies.begin(), child.dependencies.end(), task.spec.id) == child.dependencies.end())
                            child.dependencies.push_back(task.spec.id);
                        if (!expanded.emplace(child.id, TaskRecord{child}).second) throw AgentError("decomposition", "Duplicate child task ID");
                        new_ids.push_back(child.id);
                    }
                    validate_graph(expanded, agents);
                    std::set<std::string> allowed;
                    for (auto const &dependency : task.spec.dependencies) {
                        auto const &refs = tasks.at(dependency).outputs;
                        allowed.insert(refs.begin(), refs.end());
                    }
                    for (auto const &draft : result.outputs) {
                        if (!binding.descriptor.output_formats.contains(draft.format))
                            throw AgentError("format", "Agent returned an undeclared output format");
                    }
                    auto outputs = state->outputs->prepare(id, task.spec.id, task.spec.agent,
                        task.spec.output_format, result.outputs, allowed);
                    // Reserved task metadata is committed with agent patches and registry publication.
                    Json references = Json::array();
                    for (auto const &output : outputs) references.push_back({{"id", output.id}, {"name", output.name}, {"format", output.format}});
                    auto context_task = id + "/" + task.spec.id;
                    result.context.push_back({{Scope::Task, context_task, "outputs"}, references, 0});
                    result.context.push_back({{Scope::Task, context_task, "dependencies"}, task.spec.dependencies, 0});
                    state->context->commit(task.spec.agent, context_task, binding.access, result.context, [&] {
                        if (state->stop.stop_requested()) throw AgentError("cancelled", "Workflow cancelled");
                        if (Clock::now() >= attempt.deadline) throw AgentError("timeout", "Result validation exceeded task deadline", true);
                        state->outputs->publish(outputs);
                    });
                    for (auto const &output : outputs) task.outputs.push_back(output.id);
                    task.state = TaskState::Completed;
                    task.error.clear();
                    task.error_code.clear();
                    state->event("task-completed", &task);
                    try { state->bus->send(task.spec.id, "host", MessageKind::Completed, {{"outputs", task.outputs}}, {}); }
                    catch (std::exception const &) {}
                    state->bus->register_tasks(new_ids);
                    for (auto const &child_id : new_ids) {
                        auto child = expanded.at(child_id);
                        tasks.emplace(child_id, child);
                        state->event("task-added", &child);
                    }
                } catch (AgentError const &error) {
                    if (error.retryable && task.attempts < task.spec.max_attempts && !state->stop.stop_requested()) {
                        task.state = TaskState::Assigned;
                        task.error_code = error.code;
                        task.error = error.what();
                        ready_at[task.spec.id] = Clock::now() + task.spec.retry_delay * (1u << (task.attempts - 1));
                        state->event("task-retry", &task, task.error);
                    } else fail(task, state->stop.stop_requested() ? "cancelled" : error.code, error.what());
                } catch (std::exception const &error) {
                    fail(task, "invalid-result", error.what());
                }
                it = active.erase(it);
            }
            std::map<std::string, unsigned> load;
            for (auto const &[task_id, attempt] : active) ++load[tasks.at(task_id).spec.agent];
            for (auto &[task_id, task] : tasks) {
                if (task.state != TaskState::Pending && task.state != TaskState::Assigned) continue;
                if (state->stop.stop_requested()) { fail(task, "cancelled", "Workflow cancelled"); continue; }
                bool blocked = false, failed = false;
                for (auto const &dependency : task.spec.dependencies) {
                    auto status = tasks.at(dependency).state;
                    failed |= status == TaskState::Failed;
                    blocked |= status != TaskState::Completed;
                }
                if (failed) { fail(task, "dependency", "A required task failed"); continue; }
                if (blocked || active.size() >= state->concurrency || Clock::now() < ready_at[task_id]) continue;
                if (task.state == TaskState::Pending) {
                    task.spec.agent = assign(task.spec, agents, load);
                    task.state = TaskState::Assigned;
                    state->event("task-assigned", &task);
                }
                auto binding = agents.at(task.spec.agent);
                std::set<std::string> dependencies;
                for (auto const &dependency : task.spec.dependencies) dependencies.insert(id + "/" + dependency);
                AgentRequest request{task.spec, ++task.attempts, binding.model,
                    state->context->view(task.spec.agent, id + "/" + task_id, dependencies, binding.access), {}, id + "/" + task_id};
                for (auto const &dependency : task.spec.dependencies) {
                    for (auto const &ref : tasks.at(dependency).outputs) request.inputs.push_back(state->outputs->get(ref));
                }
                Attempt attempt;
                attempt.deadline = Clock::now() + task.spec.timeout;
                auto token = attempt.stop.get_token();
                auto deadline = attempt.deadline;
                auto bus = state->bus;
                attempt.future = std::async(std::launch::async,
                    [binding, request = std::move(request), token, deadline, bus]() {
                        try {
                            auto agent = binding.factory();
                            if (!agent || agent->descriptor().id != binding.descriptor.id)
                                throw AgentError("factory", "Agent factory changed identity");
                            ExecutionContext execution(request.task.id, request.attempt, bus, token, deadline);
                            return agent->run(request, execution);
                        } catch (std::exception const &) { throw; }
                        catch (...) { throw AgentError("agent", "Agent threw an unknown exception"); }
                    });
                active.emplace(task_id, std::move(attempt));
                ++load[task.spec.agent];
                task.state = TaskState::Running;
                state->event("task-running", &task);
            }
            if (active.empty() && std::all_of(tasks.begin(), tasks.end(), [](auto const &entry) { return terminal(entry.second.state); })) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto success = std::all_of(tasks.begin(), tasks.end(), [](auto const &entry) { return entry.second.state == TaskState::Completed; });
        {
            std::lock_guard lock(state->mutex);
            state->view.state = state->stop.stop_requested() ? WorkflowState::Cancelled : success ? WorkflowState::Completed : WorkflowState::Failed;
        }
        state->event("workflow-finished");
    } catch (std::exception const &error) {
        for (auto &[key, attempt] : active) attempt.stop.request_stop();
        for (auto &[key, attempt] : active) attempt.future.wait();
        for (auto &[key, task] : tasks) if (!terminal(task.state)) fail(task, "workflow", error.what());
        {
            std::lock_guard lock(state->mutex);
            state->view.state = state->stop.stop_requested() ? WorkflowState::Cancelled : WorkflowState::Failed;
            state->view.error = error.what();
        }
        state->event("workflow-failed", nullptr, error.what());
    }
    if (context_acquired) {
        try {
            if (plan_accepted) state->context->record_iteration(state->snapshot(), prompt);
        } catch (std::exception const &error) { state->event("context-warning", nullptr, error.what()); }
        state->context->end_workflow(id);
    }
    return state->snapshot();
}
} // namespace Inkscape::AI
