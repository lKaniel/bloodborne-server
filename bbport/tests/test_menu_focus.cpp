// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the focus on our button screens follows their grid (bbport_menu_focus.h). Kaniel
// 2026-10-29, the team screen: Up went to the button on the left, Left from a row's first button to
// the row above, and the mouse lost to the old focus when it pointed at a button drawn before it.
#include <cassert>
#include <cstdio>
#include <vector>
#include "gpu/shim/bbport_menu_focus.h"

using namespace BbMenuFocus;

namespace {

/// The team screen at 1080p as in Kaniel's picture (2026-10-29), in the order it draws them:
/// the lists' buttons, the two over the row, then the row.
enum : int { Friends, ByName, NewCode, Join, Invaders, Leave, Begin, World, Deaths };
const std::vector<Item> Picture{
    {521, 479, 707, 517}, {723, 479, 963, 517}, {1580, 479, 1821, 517}, // Friends, by name, new code
    {365, 794, 649, 851}, {836, 794, 1120, 851},                        // Join, Invaders
    {365, 869, 649, 926}, {679, 869, 963, 926}, {992, 869, 1277, 926}, {1306, 869, 1591, 926},
};

void UpAndDownChangeRows() {
    // Up from Begin: the button over it (Invaders), not Leave on its left.
    assert(Step(Picture, Begin, Dir::Up) == Invaders);
    assert(Step(Picture, Leave, Dir::Up) == Join);
    assert(Step(Picture, Deaths, Dir::Up) == Invaders); // the nearest over the row's right end
    // Invaders sits over the gap between Begin and the world: one of the two under it.
    assert(Step(Picture, Invaders, Dir::Down) == Begin || Step(Picture, Invaders, Dir::Down) == World);
    assert(Step(Picture, Join, Dir::Down) == Leave);
    // From the lists' buttons down to the buttons over the row, and on up from there.
    assert(Step(Picture, ByName, Dir::Down) == Invaders);
    assert(Step(Picture, Join, Dir::Up) == Friends);
    // Past the bottom round to the top, past the top round to the bottom.
    assert(Step(Picture, Begin, Dir::Down) == ByName);
    assert(Step(Picture, Friends, Dir::Up) == Leave);
}

void LeftAndRightStayInTheRow() {
    assert(Step(Picture, Begin, Dir::Left) == Leave);
    assert(Step(Picture, Begin, Dir::Right) == World);
    // From the row's first button Left goes round to its last, not to the row above.
    assert(Step(Picture, Leave, Dir::Left) == Deaths);
    assert(Step(Picture, Deaths, Dir::Right) == Leave);
    assert(Step(Picture, Join, Dir::Right) == Invaders);
    assert(Step(Picture, Invaders, Dir::Right) == Join);
    assert(Step(Picture, NewCode, Dir::Left) == ByName);
}

void ButtonsThatCannotBePressedArePassedOver() {
    std::vector<Item> items = Picture;
    items[Begin].enabled = false; // a member's Begin: only the leader begins
    assert(Step(items, Leave, Dir::Right) == World);
    assert(Step(items, Invaders, Dir::Down) == World); // the one under it that can be pressed
    assert(Step(items, Leave, Dir::Next) == World);
    // A button alone in its row stays where it is.
    const std::vector<Item> one{{0, 0, 100, 40}};
    assert(Step(one, 0, Dir::Left) == 0 && Step(one, 0, Dir::Up) == 0);
    assert(Step(one, -1, Dir::Down) == 0);
    assert(Step({}, 0, Dir::Down) == -1);
}

void TheMouseWinsWhereverItsButtonIsDrawn() {
    // The focus on Begin, the mouse moved onto Friends (drawn before it): Friends.
    assert(Settle(Picture, Begin, Friends, Dir::None) == Friends);
    assert(Settle(Picture, Friends, Deaths, Dir::None) == Deaths);
    assert(Settle(Picture, Begin, -1, Dir::None) == Begin);
    assert(Settle(Picture, Begin, -1, Dir::Up) == Invaders);
}

/// The new team screen (2026-10-29): the settings button in the top right corner, the lists'
/// buttons, a remove button on a card, the row of three.
void TheNewScreen() {
    enum : int { Settings, Remove, Friends2, ByName2, NewCode2, Leave2, Begin2, World2 };
    const std::vector<Item> items{
        {1716, 26, 1776, 86},                                                      // settings
        {700, 360, 870, 398},                                                      // remove (card 2)
        {521, 479, 707, 517},  {723, 479, 963, 517},  {1580, 479, 1821, 517},       // lists
        {528, 869, 812, 926},  {836, 869, 1120, 926}, {1144, 869, 1428, 926},      // the row
    };
    assert(Step(items, Begin2, Dir::Up) == ByName2);
    assert(Step(items, World2, Dir::Up) == NewCode2);
    assert(Step(items, NewCode2, Dir::Up) == Remove);
    assert(Step(items, Remove, Dir::Up) == Settings);
    assert(Step(items, Settings, Dir::Down) == Remove);
    assert(Step(items, Settings, Dir::Up) == World2); // round to the bottom row, nearest under it
    assert(Step(items, Leave2, Dir::Left) == World2);
}

} // namespace

int main() {
    UpAndDownChangeRows();
    LeftAndRightStayInTheRow();
    ButtonsThatCannotBePressedArePassedOver();
    TheMouseWinsWhereverItsButtonIsDrawn();
    TheNewScreen();
    std::puts("Menu focus: PASS (rows, columns, round the ends, passed-over buttons, mouse first)");
}
