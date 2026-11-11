// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_DESIGN_AGENTS_H
#define INKSCAPE_AI_DESIGN_AGENTS_H
#include "agent.h"
namespace Inkscape::AI {
// Transport services are injected. Neither agent owns credentials or a document.
using DesignModel = std::function<std::string(ModelConfig const &, std::string const &, Json const &, ExecutionContext &)>;
using AssetSearch = std::function<Json(std::string const &, ExecutionContext &)>;
class PlannerAgent final : public Agent {
public:
    explicit PlannerAgent(DesignModel model) : _model(std::move(model)) {}
    AgentDescriptor descriptor() const override;
protected:
    AgentResult execute(AgentRequest const &, ExecutionContext &) override;
private:
    DesignModel _model;
};
class AssetDiscoveryAgent final : public Agent {
public:
    AssetDiscoveryAgent(DesignModel model, AssetSearch search) : _model(std::move(model)), _search(std::move(search)) {}
    AgentDescriptor descriptor() const override;
protected:
    AgentResult execute(AgentRequest const &, ExecutionContext &) override;
private:
    DesignModel _model;
    AssetSearch _search;
};
Json parse_design_json(std::string text);
// Normalizes external metadata; never accepts model-generated sources/URLs.
Json commons_assets(Json const &response);
bool commons_page(std::string const &url);
std::string design_preview_prompt(std::string const &prompt, Json const &plan, Json const &assets);
} // namespace Inkscape::AI
#endif
