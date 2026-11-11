// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_OUTPUT_REGISTRY_H
#define INKSCAPE_AI_OUTPUT_REGISTRY_H
#include "workflow-types.h"
#include <functional>
#include <mutex>

namespace Inkscape::AI {
class Orchestrator;
class OutputRegistry {
public:
    using Validator = std::function<std::string(std::string const &)>;
    OutputRegistry();
    // Host registration only. A format must have a validator before use.
    void register_format(std::string format, Validator validator);
    Output get(std::string const &id) const;
    std::vector<Output> list(std::string const &workflow = {}) const;
    // Only validated SVG/composition outputs can enter the existing preview path.
    std::string preview_svg(std::string const &id) const;
    void save(std::string const &path, std::string const &project) const;
    void load(std::string const &path, std::string const &project);
private:
    friend class Orchestrator;
    std::vector<Output> prepare(std::string const &workflow, std::string const &task, std::string const &agent,
                                std::string const &expected_format, std::vector<OutputDraft> const &drafts,
                                std::set<std::string> const &allowed_parents) const;
    void publish(std::vector<Output> const &outputs);
    mutable std::mutex _mutex;
    std::map<std::string, Validator> _validators;
    std::map<std::string, Output> _outputs;
    std::size_t _bytes = 0;
};
} // namespace Inkscape::AI
#endif
