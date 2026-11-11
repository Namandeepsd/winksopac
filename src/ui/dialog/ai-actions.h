// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_ACTIONS_H
#define INKSCAPE_UI_DIALOG_AI_ACTIONS_H
#include <string>
#include <optional>
#include <vector>
#include <set>
#include <nlohmann/json.hpp>
class SPDocument;
class SPItem;
namespace Inkscape::UI::Dialog {
// Snapshot-scoped, allowlisted document operations. Caller owns freshness/undo.
class AIActions {
public:
    struct Plan {
        nlohmann::json commands = nlohmann::json::array();
        std::string reason, clarification;
    };
    AIActions(SPDocument &, std::vector<SPItem *> const &selection);
    std::string catalog() const;
    static std::string instructions();
    // Narrow, standalone page-fit requests can bypass generation in any UI mode.
    static std::optional<std::string> page_fit_request(std::string const &);
    static void fit_page(SPDocument &, std::vector<SPItem *> const &selection, bool drawing);
    std::string reference_png(SPDocument &) const;
    Plan parse(std::string const &) const;
    std::vector<std::string> apply(SPDocument &, Plan const &) const;
    std::string summary(Plan const &) const;
private:
    nlohmann::json _catalog;
    std::set<std::string> _ids, _path_ids;
    std::vector<std::string> _selection;
    int _page = -1;
};
}
#endif
