// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_ORCHESTRATOR_H
#define INKSCAPE_AI_ORCHESTRATOR_H
#include "agent.h"
#include "context-manager.h"
#include "output-registry.h"
#include <future>

namespace Inkscape::AI {
// Decomposition is an injected policy, never agent-specific branches in the scheduler.
// A future Planner can also return child tasks through AgentResult::tasks.
using TaskDecomposer = std::function<std::vector<TaskSpec>(std::string const &, ContextSnapshot const &)>;
class Orchestrator {
public:
    Orchestrator(std::shared_ptr<AgentRegistry>, std::shared_ptr<ContextManager>,
                 std::shared_ptr<OutputRegistry>, unsigned concurrency = 4);
    ~Orchestrator();
    Orchestrator(Orchestrator const &) = delete;
    Orchestrator &operator=(Orchestrator const &) = delete;
    // One workflow per instance. Retain the future and poll it from GTK; never
    // call get() on the UI thread until ready. No worker accesses live documents.
    std::future<WorkflowSnapshot> start(std::string prompt, TaskDecomposer decompose);
    std::future<WorkflowSnapshot> start(std::string prompt, std::vector<TaskSpec> tasks);
    void cancel();
    WorkflowSnapshot snapshot() const;
    std::vector<Message> messages() const;
private:
    struct State;
    std::shared_ptr<State> _state;
    static WorkflowSnapshot run(std::shared_ptr<State>, std::string, TaskDecomposer);
};
} // namespace Inkscape::AI
#endif
