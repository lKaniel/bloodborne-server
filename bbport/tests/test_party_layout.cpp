// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: party death's HUD keeps clear of the game's own HUD (bbport_party_layout.h): at
// 1080p and 4K, in a small window (where the overlay's scale stops at 0.75) and with the game's
// frame letterboxed or pillarboxed.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "gpu/shim/bbport_party_layout.h"

using namespace BbPartyLayout;

namespace {

/// The bottom of the downed player's own screen: the give-up bar's lower edge.
float DownBottom(const Rect& game, float scale) {
    return DownRingY(game, scale) + (DownBarBelow + DownBarHeight) * scale;
}

void ClearOfTheGamesHud(const Rect& game, float scale) {
    // The list starts below the game's quick items and inside the game's frame, not in a bar.
    assert(ListTop(game) >= game.y + game.h * GameQuickItemsBottom + 5.0f * scale);
    assert(ListLeft(game, scale) >= game.x);
    // The downed screen ends above the game's notices, with its ring below the list's top.
    assert(DownBottom(game, scale) <= game.y + game.h * GameNoticeTop);
    assert(DownRingY(game, scale) - DownRingRadius * scale >= ListTop(game));
    // The revive prompt (a band from 10 px above its line) stays below the notices.
    assert(RevivePromptY(game) - 10.0f * scale >= game.y + game.h * GameNoticeBottom);
}

} // namespace

int main() {
    ClearOfTheGamesHud({0, 0, 1920, 1080}, Scale(1080));
    ClearOfTheGamesHud({0, 0, 3840, 2160}, Scale(2160));
    ClearOfTheGamesHud({0, 0, 1280, 720}, Scale(720));
    ClearOfTheGamesHud({0, 0, 960, 540}, Scale(540));
    // 16:10: 60 px bars above and below the frame; everything moves down with the game's HUD.
    ClearOfTheGamesHud({0, 60, 1920, 1080}, Scale(1200));
    assert(ListTop({0, 60, 1920, 1080}) == 60.0f + ListTop({0, 0, 1920, 1080}));
    assert(DownRingY({0, 60, 1920, 1080}, 1.0f) == 60.0f + DownRingY({0, 0, 1920, 1080}, 1.0f));
    // 21:9: bars left and right; the list moves in with the game's HUD.
    ClearOfTheGamesHud({440, 0, 2560, 1440}, Scale(1440));
    assert(ListLeft({440, 0, 2560, 1440}, 1.0f) == 440.0f + ListLeft({0, 0, 2560, 1440}, 1.0f));
    // Kaniel 2026-10-24: a 21:9 frame (the game sees more to the sides): the game's screen is its
    // middle at its height; the list goes to the left edge with the player's bars, the rest stays.
    const Rect wide{0, 0, 2560, 1080};
    const Rect stage = Stage(wide);
    assert(stage.x == 320.0f && stage.w == 1920.0f && stage.y == 0.0f && stage.h == 1080.0f);
    const Shift shift{320.0f, 0.0f}; // BbGuestHooks::HudEdgeShift at 2560x1080
    assert(ListLeft(wide, 1.0f, shift) == ListLeft({0, 0, 1920, 1080}, 1.0f));
    assert(ListLeft(wide, 1.0f) == 320.0f + ListLeft({0, 0, 1920, 1080}, 1.0f) && "no hooks: in the middle");
    assert(ListTop(wide, shift) == ListTop({0, 0, 1920, 1080}));
    assert(DownRingY(wide, 1.0f) == DownRingY({0, 0, 1920, 1080}, 1.0f));
    assert(HudX(wide, 300.0f, shift) == 300.0f && HudY(wide, 330.0f, shift) == 330.0f);
    ClearOfTheGamesHud(stage, 1.0f);
    // 4:3 (more above and below): the screen is the frame's middle at its width; the list goes up.
    const Rect tall{0, 0, 1600, 1200};
    const Rect middle = Stage(tall);
    assert(middle.x == 0.0f && middle.w == 1600.0f && middle.h == 900.0f && middle.y == 150.0f);
    const Shift up{0.0f, 180.0f};
    assert(std::abs(ListTop(tall, up) - std::round(0.225f * 900.0f)) < 1.0f);
    assert(std::abs(RevivePromptY(tall) - (150.0f + std::round(0.74f * 900.0f))) < 1.0f);
    assert(Stage({0, 0, 1920, 1080}).x == 0.0f && Stage({0, 0, 1280, 720}).w == 1280.0f);

    // Kaniel 2026-10-26: the team screen in a narrow window (4:3 960x720, 16:10 1152x720: the overlay's
    // scale stops at 0.75) ran off both sides; it scales down to the width. Its cards, its button row
    // and its window fit; wider frames keep the overlay's scale.
    for (const Rect frame : {Rect{0, 0, 960, 720}, Rect{0, 0, 1152, 720}, Rect{0, 0, 1024, 768},
                             Rect{0, 0, 960, 540}, Rect{0, 0, 1280, 720}, Rect{0, 0, 1920, 1080},
                             Rect{0, 0, 2560, 720}, Rect{0, 0, 1440, 1080}}) {
        const float overlay = Scale(Stage(frame).h);
        const float s = TeamScreenScale(overlay, frame.w);
        assert(s <= overlay && s > 0.0f);
        const float window = TeamWindowWidth * s;
        assert(window <= frame.w - TeamWindowMargin * s + 0.01f);
        assert(TeamButtonsWidth * s <= window - 2 * TeamWindowInset * s);
        assert(TeamCardsWidth * s <= window - 2 * TeamWindowInset * s);
    }
    assert(TeamScreenScale(1.0f, 1920.0f) == 1.0f && TeamScreenScale(0.75f, 1280.0f) == 0.75f);
    assert(TeamScreenScale(0.75f, 960.0f) < 0.75f);
    // The YOU ARE DOWN banner (the overlay's, never the game's NIGHTMARE SLAIN): as a player goes
    // down, not while it stays down; fades in, holds, fades out; above the ring, inside the frame.
    assert(DownBannerBegins(false, true));
    assert(!DownBannerBegins(true, true) && !DownBannerBegins(true, false));
    assert(DownBannerAlpha(0.0f) == 0.0f && DownBannerAlpha(1.0f) == 1.0f);
    assert(DownBannerAlpha(DownBannerSeconds - 0.25f) > 0.0f);
    assert(DownBannerAlpha(DownBannerSeconds) == 0.0f && DownBannerAlpha(-1.0f) == 0.0f);
    for (const Rect& game : {Rect{0, 0, 1920, 1080}, Rect{0, 60, 1920, 1080}, Rect{0, 0, 3840, 2160}}) {
        const float scale = Scale(game.h);
        assert(DownBannerTop(game, scale) >= game.y);
        assert(DownBannerTop(game, scale) + DownBannerSize * scale <
               DownRingY(game, scale) - DownRingRadius * scale);
    }
    assert(std::strcmp(DownBannerText("You are down"), "YOU ARE DOWN") == 0);
    assert(std::strcmp(DownBannerText("Niedergestreckt"), "Niedergestreckt") == 0);
    std::puts("Party layout: PASS (1080p, 4K, 720p, small window, letterboxed, pillarboxed, 21:9, 4:3, "
              "narrow team screen)");
}
