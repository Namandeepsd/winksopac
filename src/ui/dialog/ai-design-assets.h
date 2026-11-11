// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_DESIGN_ASSETS_H
#define INKSCAPE_UI_DIALOG_AI_DESIGN_ASSETS_H
#include "ai/design-agents.h"
namespace Inkscape::UI::Dialog {
struct DesignAsset {
    std::string id, title, source, license, attribution;
    std::string svg, png, error;
    int width = 0, height = 0;
    bool available() const { return !png.empty() && error.empty(); }
};
using AssetDownload = std::function<std::string(std::string const &, Inkscape::AI::ExecutionContext &)>;
bool commons_download_url(std::string const &);
DesignAsset prepare_design_asset(Inkscape::AI::Json const &, std::string id, AssetDownload const &, Inkscape::AI::ExecutionContext &);
// Only accepts a restricted generated layout and prepared local assets. Never
// interpret a model-supplied URL or let a model supply embedded image bytes.
std::string compose_design_assets(std::string const &layout, std::vector<DesignAsset> const &);
std::string asset_layout_prompt(std::vector<DesignAsset> const &);
}
#endif
