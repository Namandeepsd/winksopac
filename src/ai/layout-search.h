// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_LAYOUT_SEARCH_H
#define INKSCAPE_AI_LAYOUT_SEARCH_H
#include <cstddef>
#include <functional>
#include <vector>
namespace Inkscape::AI {
// Integer grid coordinates; callers map these to their page's coordinate system.
struct LayoutBox { int x, y, width, height; };
struct LayoutVariable { std::vector<LayoutBox> domain; };
enum class LayoutStatus { Solved, Unsatisfiable, LimitReached };
struct LayoutResult {
    LayoutStatus status = LayoutStatus::Unsatisfiable;
    std::vector<LayoutBox> boxes; // Input variable order; empty unless solved.
    std::size_t visited = 0, backtracks = 0, pruned = 0;
};
LayoutResult solve_layout(int width, int height, int margin, int gap,
                          std::vector<LayoutVariable> const &variables,
                          std::size_t node_limit = 10000,
                          std::function<void()> checkpoint = {});
}
#endif
