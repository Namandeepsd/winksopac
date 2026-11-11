// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_AGENT_H
#define INKSCAPE_AI_AGENT_H
#include "message-bus.h"
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

namespace Inkscape::AI {
struct AgentDescriptor {
    std::string id, name, description;
    std::set<std::string> capabilities, input_formats, output_formats;
    bool can_decompose = false;
};
struct AgentRequest {
    TaskSpec task;
    unsigned attempt = 1;
    ModelConfig model;
    ContextSnapshot context;
    std::vector<Output> inputs; // Only completed dependency outputs, copied.
    std::string context_task; // Workflow-qualified task namespace for ContextPatch.
};
class AgentError : public std::runtime_error {
public:
    AgentError(std::string code, std::string detail, bool retryable = false)
        : std::runtime_error(std::move(detail)), code(std::move(code)), retryable(retryable) {}
    std::string code;
    bool retryable;
};
class ExecutionContext {
public:
    ExecutionContext(std::string task, unsigned attempt, std::shared_ptr<MessageBus> bus,
                     std::stop_token stop, Clock::time_point deadline);
    void checkpoint() const;
    std::stop_token stop_token() const { return _stop; }
    Clock::time_point deadline() const { return _deadline; }
    void send(std::string const &to, MessageKind kind, Json payload, std::string correlation = {});
    std::vector<Message> receive(std::uint64_t after = 0, std::chrono::milliseconds wait = std::chrono::milliseconds(0));
private:
    std::string _task, _correlation;
    std::shared_ptr<MessageBus> _bus;
    std::stop_token _stop;
    Clock::time_point _deadline;
};
class Agent {
public:
    virtual ~Agent() = default;
    virtual AgentDescriptor descriptor() const = 0;
    AgentStatus status() const { return _status.load(); }
    AgentResult run(AgentRequest const &request, ExecutionContext &execution);
protected:
    virtual AgentResult execute(AgentRequest const &, ExecutionContext &) = 0;
private:
    std::atomic<AgentStatus> _status{AgentStatus::Idle};
};
using AgentFactory = std::function<std::unique_ptr<Agent>()>;
struct AgentBinding {
    AgentDescriptor descriptor;
    AgentFactory factory; // Fresh instance per attempt; never share mutable agent state.
    ModelConfig model;
    ContextAccess access;
};
class AgentRegistry {
public:
    void add(AgentFactory factory, ModelConfig model = {}, ContextAccess access = {});
    std::map<std::string, AgentBinding> snapshot() const;
    std::vector<AgentDescriptor> discover(std::string const &capability = {}) const;
private:
    mutable std::mutex _mutex;
    std::map<std::string, AgentBinding> _agents;
};
} // namespace Inkscape::AI
#endif
