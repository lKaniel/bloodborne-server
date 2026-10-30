// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: our screens that hold the game's input take it themselves (bbport_overlay_input.h).
// Kaniel's players 2026-10-30: the travel together question could not be answered.
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "gpu/shim/bbport_overlay_input.h"

using namespace BbOverlayInput;

namespace {

void TravelQuestionTakesItsAnswer() {
    const Screens travel{.travel = true};
    assert(HoldsGame(travel));
    assert(TakesEvents(travel)); // Cross / Enter / the d-pad / a click reach its buttons
    assert(ShowsCursor(travel)); // its buttons take the mouse, fullscreen too
}

void ScreensWithButtonsTakeEvents() {
    // Every screen with something to choose that holds the game's input gets the events too, or
    // the player is stuck on it.
    for (const Screens s : {Screens{.menu = true}, Screens{.quit = true}, Screens{.team = true},
                            Screens{.map = true}, Screens{.travel = true}}) {
        assert(HoldsGame(s));
        assert(TakesEvents(s));
    }
}

void LoadCoverHoldsTheGameOnly() {
    const Screens joining{.joining = true};
    assert(HoldsGame(joining));
    assert(!TakesEvents(joining));
    assert(!ShowsCursor(joining));
}

void NothingOpenLeavesTheGame() {
    assert(!HoldsGame({}));
    assert(!TakesEvents({}));
    assert(!ShowsCursor({}));
}

} // namespace

int main() {
    TravelQuestionTakesItsAnswer();
    ScreensWithButtonsTakeEvents();
    LoadCoverHoldsTheGameOnly();
    NothingOpenLeavesTheGame();
    std::printf("overlay-input-test: ok\n");
    return 0;
}
