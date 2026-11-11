// SPDX-License-Identifier: GPL-2.0-or-later
#include "output-registry.h"
#include "svg-validation.h"
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <glib.h>

namespace Inkscape::AI {
OutputRegistry::OutputRegistry()
{
    register_format("image/svg+xml", validate_generated_svg);
    register_format("application/json", [](std::string const &data) {
        auto value = Json::parse(data, [](int depth, auto, Json &) {
            if (depth > 64) throw std::runtime_error("Output JSON nesting exceeds limit");
            return true;
        });
        require_json(value);
        return value.dump();
    });
    register_format("text/plain", [](std::string const &data) {
        if (data.find('\0') != std::string::npos || !g_utf8_validate(data.data(), data.size(), nullptr))
            throw std::runtime_error("Output is not valid UTF-8 text");
        return data;
    });
}
void OutputRegistry::register_format(std::string format, Validator validator)
{
    if (format.empty() || format.size() > 128 || !validator) throw std::invalid_argument("Invalid output validator");
    std::lock_guard lock(_mutex);
    if (!_validators.emplace(std::move(format), std::move(validator)).second)
        throw std::invalid_argument("Output validator already registered");
}
std::vector<Output> OutputRegistry::prepare(std::string const &workflow, std::string const &task,
    std::string const &agent, std::string const &expected_format, std::vector<OutputDraft> const &drafts,
    std::set<std::string> const &allowed_parents) const
{
    if (drafts.empty() || drafts.size() > 32) throw std::runtime_error("Expected 1–32 task outputs");
    std::map<std::string, Validator> validators;
    { std::lock_guard lock(_mutex); validators = _validators; }
    std::vector<Output> result;
    std::set<std::string> names;
    bool expected = false;
    for (auto const &draft : drafts) {
        require_id(draft.name);
        if (!names.insert(draft.name).second) throw std::runtime_error("Duplicate output name");
        if (draft.data.empty() || draft.data.size() > 4 * 1024 * 1024 || draft.parents.size() > 128 || draft.sources.size() > 64)
            throw std::runtime_error("Invalid or oversized output");
        if (draft.kind < OutputKind::Svg || draft.kind > OutputKind::Composition)
            throw std::runtime_error("Unknown output kind");
        if ((draft.kind == OutputKind::Svg || draft.kind == OutputKind::SvgComponent || draft.kind == OutputKind::Composition)
            && draft.format != "image/svg+xml") throw std::runtime_error("SVG output requires SVG format");
        if (draft.kind == OutputKind::Image && (!draft.format.starts_with("image/") || draft.format == "image/svg+xml"))
            throw std::runtime_error("Image output requires a raster format validator");
        if (draft.kind == OutputKind::Asset && draft.sources.empty()) throw std::runtime_error("Retrieved assets require source provenance");
        for (auto const &source : draft.sources) {
            if (source.uri.empty() || source.uri.size() > 4096 || source.attribution.size() > 4096 || source.license.size() > 4096)
                throw std::runtime_error("Invalid output source");
        }
        for (auto const &parent : draft.parents) {
            if (!allowed_parents.contains(parent)) throw std::runtime_error("Output references an inaccessible parent");
        }
        auto validator = validators.find(draft.format);
        if (validator == validators.end()) throw std::runtime_error("No validator for output format: " + draft.format);
        Output output;
        static_cast<OutputDraft &>(output) = draft;
        try { output.data = validator->second(draft.data); }
        catch (std::exception const &) { throw; }
        catch (...) { throw std::runtime_error("Output validator threw an unknown exception"); }
        if (output.data.empty() || output.data.size() > 4 * 1024 * 1024) throw std::runtime_error("Invalid normalized output size");
        output.id = workflow + "/" + task + "/" + draft.name;
        output.workflow = workflow;
        output.task = task;
        output.agent = agent;
        expected |= draft.format == expected_format;
        result.push_back(std::move(output));
    }
    if (!expected) throw std::runtime_error("Agent did not return the expected output format");
    return result;
}
void OutputRegistry::publish(std::vector<Output> const &outputs)
{
    std::lock_guard lock(_mutex);
    auto next = _outputs;
    auto bytes = _bytes;
    for (auto const &output : outputs) {
        bytes += output.data.size();
        if (bytes > 32 * 1024 * 1024 || next.size() >= 4096) throw std::runtime_error("Output registry budget exceeded");
        if (!next.emplace(output.id, output).second) throw std::runtime_error("Output already published");
    }
    _outputs.swap(next);
    _bytes = bytes;
}
Output OutputRegistry::get(std::string const &id) const
{
    std::lock_guard lock(_mutex);
    return _outputs.at(id);
}
std::vector<Output> OutputRegistry::list(std::string const &workflow) const
{
    std::lock_guard lock(_mutex);
    std::vector<Output> result;
    for (auto const &[id, output] : _outputs) {
        if (workflow.empty() || output.workflow == workflow) result.push_back(output);
    }
    return result;
}
std::string OutputRegistry::preview_svg(std::string const &id) const
{
    auto output = get(id);
    if (output.kind != OutputKind::Svg && output.kind != OutputKind::SvgComponent && output.kind != OutputKind::Composition)
        throw std::runtime_error("This output is not a canvas proposal");
    return validate_generated_svg(output.data);
}
void OutputRegistry::save(std::string const &path, std::string const &project) const
{
    std::lock_guard lock(_mutex);
    Json document = {{"version", 1}, {"project", project}, {"outputs", Json::array()}};
    for (auto const &[id, output] : _outputs) {
        // Base64 handles binary image formats without interpreting them as UTF-8.
        g_autofree char *bytes = g_base64_encode(reinterpret_cast<guchar const *>(output.data.data()), output.data.size());
        Json sources = Json::array();
        for (auto const &source : output.sources) sources.push_back({{"uri", source.uri}, {"attribution", source.attribution}, {"license", source.license}});
        document["outputs"].push_back({{"id", id}, {"workflow", output.workflow}, {"task", output.task},
            {"agent", output.agent}, {"name", output.name}, {"format", output.format}, {"kind", int(output.kind)},
            {"data", bytes}, {"sources", sources}, {"parents", output.parents}});
    }
    auto data = document.dump();
    if (data.size() > 48 * 1024 * 1024) throw std::runtime_error("Output archive exceeds size limit");
    g_autoptr(GError) error = nullptr;
    if (!g_file_set_contents_full(path.c_str(), data.data(), data.size(),
        GFileSetContentsFlags(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600, &error))
        throw std::runtime_error(error->message);
}
void OutputRegistry::load(std::string const &path, std::string const &project)
{
    if (std::filesystem::file_size(path) > 48 * 1024 * 1024) throw std::runtime_error("Output archive too large");
    std::ifstream input(path);
    auto document = Json::parse(input, [](int depth, auto, Json &) {
        if (depth > 64) throw std::runtime_error("Output archive nesting exceeds limit");
        return true;
    });
    if (document.at("version") != 1 || document.at("project") != project ||
        !document.at("outputs").is_array() || document.at("outputs").size() > 4096)
        throw std::runtime_error("Unsupported output archive or different project");
    std::set<std::string> ids;
    for (auto const &item : document.at("outputs")) {
        if (!ids.insert(item.at("id").get<std::string>()).second) throw std::runtime_error("Duplicate archived output");
    }
    std::vector<Output> restored;
    for (auto const &item : document.at("outputs")) {
        OutputDraft draft;
        draft.name = item.at("name");
        draft.format = item.at("format");
        draft.kind = OutputKind(item.at("kind").get<int>());
        draft.parents = item.at("parents").get<std::vector<std::string>>();
        auto encoded = item.at("data").get<std::string>();
        if (encoded.size() > 6 * 1024 * 1024) throw std::runtime_error("Archived output too large");
        gsize length = 0;
        g_autofree guchar *bytes = g_base64_decode(encoded.c_str(), &length);
        g_autofree char *canonical = g_base64_encode(bytes, length);
        if (encoded != canonical) throw std::runtime_error("Malformed archived output encoding");
        draft.data.assign(reinterpret_cast<char const *>(bytes), length);
        for (auto const &source : item.at("sources")) draft.sources.push_back({source.at("uri"), source.at("attribution"), source.at("license")});
        auto workflow = item.at("workflow").get<std::string>();
        auto task = item.at("task").get<std::string>();
        auto agent = item.at("agent").get<std::string>();
        require_id(workflow); require_id(task); require_id(agent);
        auto checked = prepare(workflow, task, agent, draft.format, {draft}, ids).front();
        if (checked.id != item.at("id").get<std::string>()) throw std::runtime_error("Archived output identity mismatch");
        restored.push_back(std::move(checked));
    }
    // Import into a fresh registry only; never replace outputs of an active run.
    std::lock_guard lock(_mutex);
    if (!_outputs.empty()) throw std::runtime_error("Load outputs into an empty registry");
    std::map<std::string, Output> next;
    std::size_t size = 0;
    for (auto const &output : restored) {
        size += output.data.size();
        if (size > 32 * 1024 * 1024) throw std::runtime_error("Output archive exceeds decoded budget");
        next.emplace(output.id, output);
    }
    std::map<std::string, int> visited;
    std::function<void(std::string const &)> visit = [&](std::string const &id) {
        if (visited[id] == 1) throw std::runtime_error("Circular archived output provenance");
        if (visited[id] == 2) return;
        visited[id] = 1;
        for (auto const &parent : next.at(id).parents) {
            if (next.at(parent).workflow != next.at(id).workflow) throw std::runtime_error("Cross-workflow archived provenance");
            visit(parent);
        }
        visited[id] = 2;
    };
    for (auto const &[id, output] : next) visit(id);
    _outputs.swap(next);
    _bytes = size;
}
} // namespace Inkscape::AI
