// SPDX-License-Identifier: GPL-2.0-or-later
#include "context-manager.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <glib.h>

namespace Inkscape::AI {
namespace {
constexpr std::size_t max_context = 8 * 1024 * 1024;
Json encode(ContextSnapshot const &data)
{
    Json entries = Json::array();
    for (auto const &[key, entry] : data.entries) {
        entries.push_back({{"scope", int(key.scope)}, {"owner", key.owner}, {"name", key.name},
            {"value", entry.value}, {"author", entry.author}, {"revision", entry.revision}});
    }
    return {{"version", 1}, {"project", data.project}, {"revision", data.revision}, {"entries", entries}};
}
void validate_key(ContextKey const &key)
{
    require_id(key.name);
    if (key.scope == Scope::Project) {
        if (!key.owner.empty()) throw std::invalid_argument("Project context cannot have an owner namespace");
    } else if (key.scope == Scope::Task) {
        auto separator = key.owner.find('/');
        if (separator == std::string::npos) throw std::invalid_argument("Task context requires workflow/task namespace");
        require_id(key.owner.substr(0, separator));
        require_id(key.owner.substr(separator + 1));
    } else if (key.scope == Scope::Agent) require_id(key.owner);
    else throw std::invalid_argument("Unknown context scope");
}
}
ContextManager::ContextManager(std::string project)
{
    if (project.empty() || project.size() > 32768) throw std::invalid_argument("Project identity is required");
    _data.project = std::move(project);
}
void ContextManager::set_project(std::string const &name, Json value)
{
    require_id(name);
    require_json(value);
    std::lock_guard lock(_mutex);
    auto next = _data;
    next.entries[{Scope::Project, {}, name}] = {std::move(value), "host", ++next.revision};
    if (next.entries.size() > 4096) throw std::runtime_error("Context entry budget exceeded");
    require_json(encode(next), max_context);
    std::swap(_data, next);
}
ContextSnapshot ContextManager::snapshot() const
{
    std::lock_guard lock(_mutex);
    return _data;
}
ContextSnapshot ContextManager::view(std::string const &agent, std::string const &task,
                                    std::set<std::string> const &dependencies, ContextAccess const &access) const
{
    std::lock_guard lock(_mutex);
    ContextSnapshot result{_data.project, _data.revision, {}};
    for (auto const &[key, entry] : _data.entries) {
        if ((key.scope == Scope::Project && access.read_project.contains(key.name)) ||
            (key.scope == Scope::Agent && key.owner == agent) ||
            (key.scope == Scope::Task && (key.owner == task || dependencies.contains(key.owner)))) {
            result.entries.emplace(key, entry);
        }
    }
    return result;
}
void ContextManager::commit(std::string const &agent, std::string const &task, ContextAccess const &access,
                            std::vector<ContextPatch> const &patches, std::function<void()> publish)
{
    require_id(agent);
    validate_key({Scope::Task, task, "task"});
    if (patches.size() > 64) throw std::invalid_argument("Too many context patches");
    std::lock_guard lock(_mutex);
    auto next = _data;
    std::set<ContextKey> changed;
    for (auto const &patch : patches) {
        auto const &key = patch.key;
        validate_key(key);
        require_json(patch.value);
        bool allowed = (key.scope == Scope::Project && access.write_project.contains(key.name)) ||
                       (key.scope == Scope::Agent && key.owner == agent) ||
                       (key.scope == Scope::Task && key.owner == task);
        if (!allowed || !changed.insert(key).second) throw std::runtime_error("Context write denied: " + key.name);
        auto it = next.entries.find(key);
        auto revision = it == next.entries.end() ? 0 : it->second.revision;
        if (revision != patch.expected_revision) throw std::runtime_error("Context revision conflict: " + key.name);
        if (it != next.entries.end() && it->second.author != agent)
            throw std::runtime_error("Cannot overwrite another author's context: " + key.name);
        next.entries[key] = {patch.value, agent, ++next.revision};
    }
    if (next.entries.size() > 4096) throw std::runtime_error("Context entry budget exceeded");
    require_json(encode(next), max_context);
    if (publish) publish();
    std::swap(_data, next);
}
void ContextManager::save(std::string const &path) const
{
    // Serialize concurrent saves from this manager, including the atomic replace.
    std::lock_guard lock(_mutex);
    auto bytes = encode(_data).dump();
    if (bytes.size() > max_context) throw std::runtime_error("Context exceeds disk budget");
    g_autoptr(GError) error = nullptr;
    if (!g_file_set_contents_full(path.c_str(), bytes.data(), bytes.size(),
        GFileSetContentsFlags(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600, &error))
        throw std::runtime_error(error->message);
}
void ContextManager::load(std::string const &path)
{
    std::lock_guard lock(_mutex);
    if (!_active_workflow.empty()) throw std::runtime_error("Cannot load context during a workflow");
    if (std::filesystem::file_size(path) > max_context) throw std::runtime_error("Context file too large");
    std::ifstream input(path);
    auto json = Json::parse(input, [](int depth, auto, Json &) {
        if (depth > 64) throw std::runtime_error("Context nesting exceeds limit");
        return true;
    });
    if (json.at("version") != 1 || json.at("project") != _data.project)
        throw std::runtime_error("Unsupported context version or different project");
    ContextSnapshot next{_data.project, json.at("revision").get<std::uint64_t>(), {}};
    auto const &entries = json.at("entries");
    if (!entries.is_array() || entries.size() > 4096) throw std::runtime_error("Invalid context entries");
    for (auto const &entry : entries) {
        ContextKey key{Scope(entry.at("scope").get<int>()), entry.at("owner"), entry.at("name")};
        validate_key(key);
        ContextEntry value{entry.at("value"), entry.at("author"), entry.at("revision")};
        require_id(value.author);
        require_json(value.value);
        if (!value.revision || value.revision > next.revision || !next.entries.emplace(key, value).second)
            throw std::runtime_error("Invalid context revision or duplicate key");
    }
    require_json(encode(next), max_context);
    std::swap(_data, next);
}
void ContextManager::begin_workflow(std::string const &id)
{
    std::lock_guard lock(_mutex);
    if (!_active_workflow.empty()) throw std::runtime_error("This project already has an active workflow");
    _active_workflow = id;
}
void ContextManager::end_workflow(std::string const &id)
{
    std::lock_guard lock(_mutex);
    if (_active_workflow == id) _active_workflow.clear();
}
void ContextManager::record_iteration(WorkflowSnapshot const &workflow, std::string const &prompt)
{
    Json iteration = {{"workflow", workflow.id}, {"prompt", prompt}, {"state", int(workflow.state)}, {"tasks", Json::array()}};
    for (auto const &[id, task] : workflow.tasks) {
        iteration["tasks"].push_back({{"id", id}, {"agent", task.spec.agent}, {"description", task.spec.description},
            {"dependencies", task.spec.dependencies}, {"state", int(task.state)}, {"outputs", task.outputs},
            {"error_code", task.error_code}, {"error", task.error}});
    }
    std::lock_guard lock(_mutex);
    auto next = _data;
    ContextKey key{Scope::Project, {}, "iterations"};
    auto previous = next.entries.find(key);
    Json iterations = previous == next.entries.end() ? Json::array() : previous->second.value;
    if (!iterations.is_array()) throw std::runtime_error("Iteration history must be an array");
    iterations.push_back(iteration);
    while (iterations.size() > 16 || (iterations.size() > 1 && iterations.dump().size() > 256 * 1024)) iterations.erase(iterations.begin());
    require_json(iterations);
    next.entries[key] = {std::move(iterations), "host", ++next.revision};
    if (next.entries.size() > 4096) throw std::runtime_error("Context entry budget exceeded");
    require_json(encode(next), max_context);
    std::swap(_data, next);
}
} // namespace Inkscape::AI
