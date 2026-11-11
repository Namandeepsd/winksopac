// SPDX-License-Identifier: GPL-2.0-or-later
#include "layout-search.h"
#include <cstdint>
#include <stdexcept>
namespace Inkscape::AI {
LayoutResult solve_layout(int width, int height, int margin, int gap,
                          std::vector<LayoutVariable> const &variables,
                          std::size_t node_limit, std::function<void()> checkpoint)
{
    if (width <= 0 || height <= 0 || margin < 0 || gap < 0 || variables.size() > 16 || !node_limit)
        throw std::invalid_argument("Invalid layout search bounds");
    LayoutResult result;
    using Domains = std::vector<std::vector<LayoutBox>>;
    Domains domains(variables.size());
    for (std::size_t i = 0; i < variables.size(); ++i) {
        if (variables[i].domain.size() > 1024) throw std::invalid_argument("Layout domain exceeds limit");
        for (auto b : variables[i].domain) {
            if (b.width > 0 && b.height > 0 && b.x >= margin && b.y >= margin &&
                std::int64_t(b.x) + b.width <= std::int64_t(width) - margin &&
                std::int64_t(b.y) + b.height <= std::int64_t(height) - margin) domains[i].push_back(b);
            else ++result.pruned;
        }
    }
    auto compatible = [gap](LayoutBox a, LayoutBox b) {
        return std::int64_t(a.x) + a.width + gap <= b.x || std::int64_t(b.x) + b.width + gap <= a.x ||
               std::int64_t(a.y) + a.height + gap <= b.y || std::int64_t(b.y) + b.height + gap <= a.y;
    };
    std::vector<bool> assigned(variables.size());
    std::vector<LayoutBox> solution(variables.size());
    bool limited = false;
    std::function<bool(Domains const &, std::size_t)> search = [&](Domains const &remaining, std::size_t depth) {
        if (checkpoint) checkpoint();
        if (depth == variables.size()) return true;
        std::size_t next = variables.size();
        for (std::size_t i = 0; i < variables.size(); ++i) if (!assigned[i]) {
            if (remaining[i].empty()) return false;
            if (next == variables.size() || remaining[i].size() < remaining[next].size()) next = i;
        }
        for (auto box : remaining[next]) {
            if (result.visited >= node_limit) { limited = true; return false; }
            ++result.visited;
            assigned[next] = true;
            solution[next] = box;
            auto filtered = remaining;
            bool viable = true;
            for (std::size_t i = 0; i < variables.size(); ++i) if (!assigned[i]) {
                filtered[i].clear();
                for (auto candidate : remaining[i]) {
                    if (compatible(box, candidate)) filtered[i].push_back(candidate);
                    else ++result.pruned;
                }
                if (filtered[i].empty()) viable = false;
            }
            if (viable && search(filtered, depth + 1)) return true;
            assigned[next] = false;
            ++result.backtracks;
            if (limited) return false;
        }
        return false;
    };
    if (search(domains, 0)) { result.status = LayoutStatus::Solved; result.boxes = std::move(solution); }
    else if (limited) result.status = LayoutStatus::LimitReached;
    return result;
}
}
