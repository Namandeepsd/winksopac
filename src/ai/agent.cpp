// SPDX-License-Identifier: GPL-2.0-or-later
#include "agent.h"
#include <algorithm>

namespace Inkscape::AI {
ExecutionContext::ExecutionContext(std::string task, unsigned attempt, std::shared_ptr<MessageBus> bus,
    std::stop_token stop, Clock::time_point deadline)
    : _task(std::move(task)), _correlation(_task + "-" + std::to_string(attempt)),
      _bus(std::move(bus)), _stop(stop), _deadline(deadline) {}
void ExecutionContext::checkpoint() const
{
    if (Clock::now() >= _deadline) throw AgentError("timeout", "Task deadline exceeded", true);
    if (_stop.stop_requested()) throw AgentError("cancelled", "Task cancelled");
}
void ExecutionContext::send(std::string const &to, MessageKind kind, Json payload, std::string correlation)
{
    checkpoint();
    // Completion/failure belongs to the host, after validation and publication.
    if (kind == MessageKind::Completed || kind == MessageKind::Failed)
        throw AgentError("message", "Agents cannot publish unvalidated terminal states");
    _bus->send(_task, to, kind, std::move(payload), correlation.empty() ? _correlation : std::move(correlation));
}
std::vector<Message> ExecutionContext::receive(std::uint64_t after, std::chrono::milliseconds wait)
{
    checkpoint();
    auto result = _bus->receive(_task, after, std::min(_deadline, Clock::now() + wait), _stop);
    checkpoint();
    return result;
}
AgentResult Agent::run(AgentRequest const &request, ExecutionContext &execution)
{
    _status = AgentStatus::Running;
    try {
        execution.checkpoint();
        auto result = execute(request, execution);
        execution.checkpoint();
        _status = AgentStatus::Idle;
        return result;
    } catch (...) {
        _status = AgentStatus::Failed;
        throw;
    }
}
void AgentRegistry::add(AgentFactory factory, ModelConfig model, ContextAccess access)
{
    if (!factory) throw std::invalid_argument("Agent factory is required");
    auto agent = factory();
    if (!agent) throw std::invalid_argument("Agent factory returned null");
    auto descriptor = agent->descriptor();
    require_id(descriptor.id);
    if (descriptor.id == "host" || descriptor.name.empty() || descriptor.description.empty() ||
        descriptor.capabilities.empty() || descriptor.input_formats.empty() || descriptor.output_formats.empty())
        throw std::invalid_argument("Incomplete agent descriptor");
    require_json(model.parameters, 8192);
    for (auto const &key : access.read_project) require_id(key);
    for (auto const &key : access.write_project) require_id(key);
    std::lock_guard lock(_mutex);
    auto id = descriptor.id;
    if (!_agents.emplace(id, AgentBinding{std::move(descriptor), std::move(factory), std::move(model), std::move(access)}).second)
        throw std::invalid_argument("Agent ID is already registered: " + id);
}
std::map<std::string, AgentBinding> AgentRegistry::snapshot() const
{
    std::lock_guard lock(_mutex);
    return _agents;
}
std::vector<AgentDescriptor> AgentRegistry::discover(std::string const &capability) const
{
    std::lock_guard lock(_mutex);
    std::vector<AgentDescriptor> result;
    for (auto const &[id, binding] : _agents) {
        if (capability.empty() || binding.descriptor.capabilities.contains(capability)) result.push_back(binding.descriptor);
    }
    return result;
}
} // namespace Inkscape::AI
