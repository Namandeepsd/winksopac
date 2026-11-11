// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_WORKFLOW_TYPES_H
#define INKSCAPE_AI_WORKFLOW_TYPES_H

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Inkscape::AI {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

enum class TaskState { Pending, Assigned, Running, Completed, Failed };
enum class AgentStatus { Idle, Running, Failed };
enum class WorkflowState { Pending, Running, Completed, Failed, Cancelled };
enum class MessageKind { InformationRequest, ContextRequest, AssetShared, Suggestion, Completed, Failed };
enum class OutputKind { Svg, SvgComponent, Image, Asset, Metadata, Intermediate, Composition };
enum class Scope { Project, Agent, Task };

struct ModelConfig {
    std::string provider, model, connection; // Opaque connection ID; never credentials.
    Json parameters = Json::object();
};

struct ContextKey {
    Scope scope = Scope::Project;
    std::string owner, name;
    auto operator<=>(ContextKey const &) const = default;
};
struct ContextEntry {
    Json value;
    std::string author;
    std::uint64_t revision = 0;
};
struct ContextSnapshot {
    std::string project;
    std::uint64_t revision = 0;
    std::map<ContextKey, ContextEntry> entries;
    Json get(ContextKey const &key) const;
};
struct ContextAccess {
    std::set<std::string> read_project, write_project;
};
struct ContextPatch {
    ContextKey key;
    Json value;
    std::uint64_t expected_revision = 0; // Zero means create, never blind overwrite.
};

struct TaskSpec {
    std::string id, agent, capability, description;
    std::string input_format = "application/json", output_format = "application/json";
    Json input = Json::object();
    std::vector<std::string> dependencies;
    unsigned max_attempts = 1;
    std::chrono::milliseconds timeout{120000}, retry_delay{100};
};
struct TaskRecord {
    TaskSpec spec;
    TaskState state = TaskState::Pending;
    unsigned attempts = 0;
    std::vector<std::string> outputs;
    std::string error_code, error;
};
struct Source {
    std::string uri, attribution, license;
};
struct OutputDraft {
    std::string name, format, data;
    OutputKind kind = OutputKind::Intermediate;
    std::vector<Source> sources;
    std::vector<std::string> parents;
};
struct Output : OutputDraft {
    std::string id, workflow, task, agent;
};
struct Message {
    std::uint64_t sequence = 0;
    MessageKind kind = MessageKind::InformationRequest;
    std::string from, to, correlation;
    Json payload = Json::object();
};
struct AgentResult {
    std::vector<OutputDraft> outputs;
    std::vector<ContextPatch> context;
    std::vector<TaskSpec> tasks; // Optional decomposition; requires registration permission.
};
struct WorkflowEvent {
    std::uint64_t sequence = 0;
    std::string kind, task, agent, detail;
    TaskState state = TaskState::Pending;
};
struct WorkflowSnapshot {
    std::string id;
    WorkflowState state = WorkflowState::Pending;
    std::map<std::string, TaskRecord> tasks;
    std::vector<WorkflowEvent> events;
    std::string error;
    double progress() const;
};

// Stable IDs are storage keys, never file paths.
void require_id(std::string const &id);
void require_json(Json const &value, std::size_t limit = 256 * 1024);
} // namespace Inkscape::AI
#endif
