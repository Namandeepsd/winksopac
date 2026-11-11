// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_CONTEXT_MANAGER_H
#define INKSCAPE_AI_CONTEXT_MANAGER_H
#include "workflow-types.h"
#include <functional>
#include <mutex>

namespace Inkscape::AI {
// Host-owned project store. Agents receive filtered value snapshots, not this object.
class ContextManager {
public:
    explicit ContextManager(std::string project);
    void set_project(std::string const &key, Json value);
    ContextSnapshot snapshot() const;
    ContextSnapshot view(std::string const &agent, std::string const &task,
                         std::set<std::string> const &dependencies, ContextAccess const &access) const;
    // Validates the entire batch before any publication. publish must not call
    // back into ContextManager. Output publication happens under this transaction.
    void commit(std::string const &agent, std::string const &task, ContextAccess const &access,
                std::vector<ContextPatch> const &patches, std::function<void()> publish = {});
    void save(std::string const &path) const;
    void load(std::string const &path); // Invalid/different-project files leave memory untouched.
    void begin_workflow(std::string const &id);
    void end_workflow(std::string const &id);
    void record_iteration(WorkflowSnapshot const &workflow, std::string const &prompt);
private:
    mutable std::mutex _mutex;
    ContextSnapshot _data;
    std::string _active_workflow;
};
} // namespace Inkscape::AI
#endif
