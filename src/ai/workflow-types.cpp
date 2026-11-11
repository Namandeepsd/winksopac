// SPDX-License-Identifier: GPL-2.0-or-later
#include "workflow-types.h"
#include <algorithm>
#include <stdexcept>

namespace Inkscape::AI {
Json ContextSnapshot::get(ContextKey const &key) const
{
    auto it = entries.find(key);
    if (it == entries.end()) throw std::runtime_error("Context is missing or not granted: " + key.name);
    return it->second.value;
}
double WorkflowSnapshot::progress() const
{
    if (tasks.empty()) return state == WorkflowState::Completed ? 1 : 0;
    auto done = std::count_if(tasks.begin(), tasks.end(), [](auto const &p) {
        return p.second.state == TaskState::Completed || p.second.state == TaskState::Failed;
    });
    return double(done) / tasks.size();
}
void require_id(std::string const &id)
{
    if (id.empty() || id.size() > 96 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    })) throw std::invalid_argument("Invalid workflow identifier: " + id);
}
void require_json(Json const &value, std::size_t limit)
{
    // dump() also rejects invalid UTF-8. Bound nesting separately on disk load.
    if (value.dump().size() > limit) throw std::invalid_argument("JSON exceeds workflow size limit");
}
} // namespace Inkscape::AI
