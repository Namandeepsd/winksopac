// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_DESIGN_SERVICES_H
#define INKSCAPE_UI_DIALOG_AI_DESIGN_SERVICES_H
#include "ai/design-agents.h"
#include "ai/orchestrator.h"
#include "ai-design-assets.h"
#include "ai-connection.h"
#include <atomic>
namespace Inkscape::UI::Dialog {
using DesignConnection = AIConnection;
Inkscape::AI::DesignModel design_model(DesignConnection connection);
Inkscape::AI::Json search_commons(std::string const &, Inkscape::AI::ExecutionContext &);
std::string download_commons_asset(std::string const &, Inkscape::AI::ExecutionContext &);
// Background owner. Destroying a panel never waits on a future; a tracked
// Inkscape async owns the session until cancellation and persistence finish.
struct DesignRun : std::enable_shared_from_this<DesignRun> {
    std::shared_ptr<Inkscape::AI::Orchestrator> workflow;
    std::shared_ptr<Inkscape::AI::OutputRegistry> outputs;
    std::atomic<bool> done{false};
    std::atomic<bool> downloading{false};
    std::stop_source download_stop;
    std::vector<DesignAsset> downloaded;
    void cancel() { workflow->cancel(); download_stop.request_stop(); }
    std::string error, warning, prompt;
    Inkscape::AI::Json plan, assets = Inkscape::AI::Json::array();
    static std::shared_ptr<DesignRun> start(DesignConnection, std::string project, std::string storage,
        std::string prompt, std::string brief, Inkscape::AI::Json canvas,
        Inkscape::AI::DesignModel model = {}, Inkscape::AI::AssetSearch search = {}, AssetDownload download = {});
};
}
#endif
