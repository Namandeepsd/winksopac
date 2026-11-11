// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai/layout-search.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace Inkscape::AI;
void check(bool ok) { if (!ok) throw std::runtime_error("Layout regression failed"); }
int main()
{
    try {
        // First branch blocks both alternatives of B; second branch permits B.
        std::vector<LayoutVariable> vars = {{{{0, 0, 4, 4}, {6, 0, 4, 4}}},
                                            {{{0, 0, 2, 2}, {2, 0, 2, 2}}}};
        auto start = std::chrono::steady_clock::now();
        auto r = solve_layout(10, 10, 0, 0, vars);
        check(r.status == LayoutStatus::Solved && r.boxes.size() == 2 && r.boxes[0].x == 6 && r.backtracks > 0);
        std::cout << "backtracking: visited=" << r.visited << " backtracks=" << r.backtracks << " pruned=" << r.pruned << '\n';
        check(solve_layout(10, 10, 0, 0, vars, 1).status == LayoutStatus::LimitReached);
        vars[0].domain.resize(1);
        check(solve_layout(10, 10, 0, 0, vars).status == LayoutStatus::Unsatisfiable);
        check(solve_layout(10, 10, 1, 0, {{{{0, 0, 2, 2}}}}).status == LayoutStatus::Unsatisfiable);
        check(solve_layout(10, 10, 0, 1, {{{{0, 0, 5, 5}}}, {{{5, 0, 5, 5}}}}).status == LayoutStatus::Unsatisfiable);
        // Exhaustively compare tiny domains with an independent pairwise oracle.
        unsigned cases = 0;
        for (int a = 1; a < 16; ++a) for (int b = 1; b < 16; ++b) {
            std::vector<LayoutVariable> problem(2);
            bool expected = false;
            for (int x = 0; x < 4; ++x) {
                if (a & (1 << x)) problem[0].domain.push_back({x, 0, 2, 1});
                if (b & (1 << x)) problem[1].domain.push_back({x, 0, 2, 1});
                for (int y = 0; y < 4; ++y)
                    if ((a & (1 << x)) && (b & (1 << y)) && (x + 2 <= y || y + 2 <= x)) expected = true;
            }
            auto answer = solve_layout(5, 1, 0, 0, problem);
            check((answer.status == LayoutStatus::Solved) == expected);
            if (expected) check(answer.boxes[0].x + 2 <= answer.boxes[1].x || answer.boxes[1].x + 2 <= answer.boxes[0].x);
            ++cases;
        }
        bool cancelled = false;
        try { solve_layout(10, 10, 0, 0, vars, 100, [] { throw std::runtime_error("cancelled"); }); }
        catch (std::runtime_error const &) { cancelled = true; }
        check(cancelled);
        bool invalid = false;
        try { solve_layout(0, 10, 0, 0, {}); } catch (std::invalid_argument const &) { invalid = true; }
        check(invalid);
        std::cout << "PASS: " << cases << " oracle cases, unsatisfiable, margins, spacing, budget, cancellation; elapsed_us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << '\n';
    } catch (std::exception const &e) { std::cerr << e.what() << '\n'; return 1; }
}
