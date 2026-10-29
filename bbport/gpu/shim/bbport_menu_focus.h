// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: where the focus goes on our button screens (bbport_overlay.cpp Team::), apart from
// ImGui so a test can hold it to the screen's grid. Kaniel 2026-10-29: on the team screen Up went to
// the button on the left and Left from a row's first button to the row above ("when you press up you
// expect to go to the higher row"); the mouse lost to the old focus when it pointed at a button drawn
// before it. The buttons are their rectangles on screen, in the order they are drawn.

#pragma once

#include <cmath>
#include <span>

namespace BbMenuFocus {

/// A button on screen: its corners, and whether it can be pressed (one that cannot is passed over).
struct Item {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool enabled = true;
};

/// A move: the d-pad's four ways, Next (the next button as drawn), or none.
enum class Dir { None, Up, Down, Left, Right, Next };

namespace Detail {

inline float CentreX(const Item& i) {
    return (i.x0 + i.x1) * 0.5f;
}
inline float CentreY(const Item& i) {
    return (i.y0 + i.y1) * 0.5f;
}

/// One row: their middles closer than half the lower button's height.
inline bool SameRow(const Item& a, const Item& b) {
    const float h = std::fmin(a.y1 - a.y0, b.y1 - b.y0);
    return std::fabs(CentreY(a) - CentreY(b)) < h * 0.5f;
}

/// How far apart two buttons are side to side: 0 when they overlap, else the gap between them.
inline float Apart(const Item& a, const Item& b) {
    return std::fmax(0.0f, std::fmax(a.x0 - b.x1, b.x0 - a.x1));
}

} // namespace Detail

/// The button the focus goes to from `from` (an index into `items`) moving `dir`. Left and Right
/// stay in the row (past its end, round to its other end); Up and Down go to the nearest row above
/// or below, to its button nearest under or over this one (past the top or the bottom, round to
/// the other end). A focus on no button goes to the first one that can be pressed.
inline int Step(std::span<const Item> items, int from, Dir dir) {
    using namespace Detail;
    const int n = static_cast<int>(items.size());
    if (n == 0) {
        return -1;
    }
    if (from < 0 || from >= n) {
        for (int i = 0; i < n; ++i) {
            if (items[static_cast<size_t>(i)].enabled) {
                return i;
            }
        }
        return 0;
    }
    const Item& here = items[static_cast<size_t>(from)];
    const auto at = [&](int i) -> const Item& { return items[static_cast<size_t>(i)]; };
    switch (dir) {
    case Dir::None:
        return from;
    case Dir::Next:
        for (int k = 1; k < n; ++k) {
            const int i = (from + k) % n;
            if (at(i).enabled) {
                return i;
            }
        }
        return from;
    case Dir::Left:
    case Dir::Right: {
        const float sign = dir == Dir::Right ? 1.0f : -1.0f;
        int best = -1, far = -1;
        float best_d = 0.0f, far_d = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (i == from || !at(i).enabled || !SameRow(here, at(i))) {
                continue;
            }
            const float d = (CentreX(at(i)) - CentreX(here)) * sign;
            if (d > 0.0f && (best < 0 || d < best_d)) {
                best = i;
                best_d = d;
            }
            if (d < 0.0f && (far < 0 || d < far_d)) {
                far = i; // the row's other end
                far_d = d;
            }
        }
        return best >= 0 ? best : far >= 0 ? far : from;
    }
    case Dir::Up:
    case Dir::Down: {
        const float sign = dir == Dir::Down ? 1.0f : -1.0f;
        // The nearest row that way, else (round) the farthest the other way.
        int row = -1, wrap = -1;
        float row_d = 0.0f, wrap_d = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (i == from || !at(i).enabled || SameRow(here, at(i))) {
                continue;
            }
            const float d = (CentreY(at(i)) - CentreY(here)) * sign;
            if (d > 0.0f && (row < 0 || d < row_d)) {
                row = i;
                row_d = d;
            }
            if (d < 0.0f && (wrap < 0 || d < wrap_d)) {
                wrap = i;
                wrap_d = d;
            }
        }
        const int anchor = row >= 0 ? row : wrap;
        if (anchor < 0) {
            return from;
        }
        // In that row: the button nearest side to side (overlapping first, then the closest middle).
        int best = -1;
        float best_apart = 0.0f, best_mid = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (!at(i).enabled || !SameRow(at(anchor), at(i))) {
                continue;
            }
            const float apart = Apart(here, at(i)), mid = std::fabs(CentreX(at(i)) - CentreX(here));
            if (best < 0 || apart < best_apart || (apart == best_apart && mid < best_mid)) {
                best = i;
                best_apart = apart;
                best_mid = mid;
            }
        }
        return best;
    }
    }
    return from;
}

/// The focus at the end of a frame: the button the mouse moved onto or clicked (`pointed`, -1 for
/// none) whatever its place in the order, else `current` moved `dir`.
inline int Settle(std::span<const Item> items, int current, int pointed, Dir dir) {
    if (pointed >= 0 && pointed < static_cast<int>(items.size())) {
        return pointed;
    }
    return Step(items, current, dir);
}

} // namespace BbMenuFocus
