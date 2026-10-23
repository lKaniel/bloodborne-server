// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Seamless red invasions on the game's side (bloodborne_invasion.h): the hostile board line kept
// out of the party, which body is the invader's, the invader's trip (search, travel, join, visit,
// home), the team's notice, kill attribution and the invader's own world kept aside.
#include <gtest/gtest.h>

#include "core/bloodborne_invasion.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_team.h"

using namespace Core::Bloodborne;

namespace {

MemberVitals Line(const std::string& member, u32 map, LampPosition at, bool hostile = false,
                  Vital vital = Vital::Up) {
    MemberVitals line;
    line.member = member;
    line.map = map;
    line.at = at;
    line.vital = vital;
    line.hostile = hostile;
    return line;
}

bbcoop::InvasionNotice Notice(u64 id, u32 in_ms = 10'000) {
    bbcoop::InvasionNotice notice;
    notice.set_id(id);
    notice.set_in_ms(in_ms);
    notice.set_invader("inv");
    notice.set_target("host");
    return notice;
}

bbcoop::InvasionStart Start(u64 id, u32 map) {
    bbcoop::InvasionStart start;
    start.set_id(id);
    start.mutable_session()->set_join("join-data");
    start.mutable_session()->set_kind(0x12345678);
    start.mutable_session()->set_sign(4);
    start.mutable_session()->set_host("host");
    start.set_target("host");
    start.set_map(map);
    start.mutable_at()->set_x(1);
    start.mutable_at()->set_y(2);
    start.mutable_at()->set_z(3);
    return start;
}

bbcoop::InvasionEnd End(u64 id, bbcoop::InvasionEnd::Reason reason) {
    bbcoop::InvasionEnd end;
    end.set_id(id);
    end.set_reason(reason);
    return end;
}

InvaderTrip::Frame At(s64 now_ms, u32 map, bool settled = true, bool in_session = false) {
    return {.now_ms = now_ms, .map = map, .settled = settled, .in_session = in_session};
}

constexpr u32 Home = 0x18000000;
constexpr u32 Target = 0x18010000;

} // namespace

// The invader's line keeps its body shown and nothing else: the party (panel, downs, revives,
// cutscene vote, boss runner) reads the board without it.
TEST(Invasion, HostileLinesLeaveTheParty) {
    VitalsBoard board;
    board.members = {Line("a", Target, {0, 0, 0}), Line("inv", Target, {5, 0, 0}, true),
                     Line("b", Target, {9, 0, 0})};
    board.wipes = {{1, Target, {"a", "inv"}}};
    const std::vector<MemberVitals> hostile = TakeHostileLines(board);
    ASSERT_EQ(hostile.size(), 1u);
    EXPECT_EQ(hostile[0].member, "inv");
    ASSERT_EQ(board.members.size(), 2u);
    EXPECT_EQ(board.members[0].member, "a");
    EXPECT_EQ(board.members[1].member, "b");
    EXPECT_EQ(board.wipes[0].members, std::vector<std::string>{"a"});
    // The party panel then never lists it.
    const PartyPanel panel = BuildPartyPanel(board.members, "a", Target, {});
    for (const auto& row : panel.members) {
        EXPECT_NE(row.member, "inv");
    }
}

TEST(Invasion, ABoardWithoutAnInvaderStaysAsItIs) {
    VitalsBoard board;
    board.members = {Line("a", Target, {0, 0, 0}), Line("b", Target, {9, 0, 0})};
    const VitalsBoard before = board;
    EXPECT_TRUE(TakeHostileLines(board).empty());
    EXPECT_EQ(board, before);
}

// The invader's body is the one its line claims; a teammate's line keeps its own body even with
// the invader standing over it.
TEST(Invasion, InvaderBodiesByTheirLines) {
    const std::vector<MemberVitals> mates = {Line("me", Target, {0, 0, 0}),
                                             Line("b", Target, {10, 0, 0})};
    const std::vector<MemberVitals> hostile = {Line("inv", Target, {11, 0, 0}, true)};
    const std::vector<std::optional<LampPosition>> bodies = {
        LampPosition{10.2f, 0, 0}, LampPosition{11.3f, 0, 0}, std::nullopt};
    EXPECT_EQ(InvaderBodies(bodies, mates, hostile, "me", Target),
              (std::vector<bool>{false, true, false}));
    // Another map, or no invader: none.
    EXPECT_EQ(InvaderBodies(bodies, mates, hostile, "me", Home),
              (std::vector<bool>{false, false, false}));
    EXPECT_EQ(InvaderBodies(bodies, mates, {}, "me", Target),
              (std::vector<bool>{false, false, false}));
    // Too far from its line: not claimed.
    const std::vector<std::optional<LampPosition>> far = {LampPosition{500, 0, 0}};
    EXPECT_EQ(InvaderBodies(far, mates, hostile, "me", Target), (std::vector<bool>{false}));
}

// A team guest left alone with the invader in the session leaves it (TeamJoin's LeaveAlone).
TEST(Invasion, TheInvaderIsNobodyToStayFor) {
    EXPECT_EQ(TeamSessionMembers(2, true), 1u);
    EXPECT_EQ(TeamSessionMembers(2, false), 2u);
    EXPECT_EQ(TeamSessionMembers(1, true), 1u);
}

// The invader joins with the server's kind and sign as they come (never a team guest's 7/6/5); the
// fallback joins as a co-operator with the server's slot and is made hostile after.
TEST(Invasion, JoinTakesTheServersKindAndSign) {
    const bbcoop::TeamSession session = Start(1, Target).session();
    EXPECT_EQ(InvasionJoinOf(session, false, 0xFFFFFFFF), (InvasionJoin{0x12345678, 4, false}));
    EXPECT_EQ(InvasionJoinOf(session, true, 0xFFFFFFFF), (InvasionJoin{0xFFFFFFFF, 4, true}));
}

// Search, notice, travel to the target's map, join, placed, the server's end, home.
TEST(InvaderTrip, TravelsJoinsVisitsAndGoesHome) {
    InvaderTrip trip;
    EXPECT_TRUE(trip.Ask(0));
    EXPECT_FALSE(trip.Ask(10)); // one at a time
    EXPECT_EQ(trip.Current(), InvaderTrip::Phase::Asked);
    trip.TakeNotice(Notice(7), 1'000);
    EXPECT_EQ(trip.Current(), InvaderTrip::Phase::Approaching);
    EXPECT_EQ(trip.ApproachUntilMs(), 11'000);
    EXPECT_EQ(trip.Tick(At(2'000, Home)), InvaderTrip::Action::None);
    EXPECT_FALSE(trip.TakeStart(Start(8, Target), 11'000)); // another invasion's
    EXPECT_TRUE(trip.TakeStart(Start(7, Target), 11'000));
    EXPECT_EQ(trip.Current(), InvaderTrip::Phase::Travelling);
    EXPECT_FALSE(trip.AwayFromHome());
    // Not settled: nothing; settled on another map: one trip.
    EXPECT_EQ(trip.Tick(At(11'100, Home, false)), InvaderTrip::Action::None);
    EXPECT_EQ(trip.Tick(At(14'000, Home)), InvaderTrip::Action::Travel);
    EXPECT_EQ(trip.Tick(At(14'100, Home)), InvaderTrip::Action::None);
    EXPECT_EQ(trip.Tick(At(20'000, 0, false)), InvaderTrip::Action::None); // the trip's load
    EXPECT_EQ(trip.Tick(At(30'000, Target)), InvaderTrip::Action::Join);
    EXPECT_EQ(trip.Current(), InvaderTrip::Phase::Joining);
    EXPECT_TRUE(trip.AwayFromHome());
    EXPECT_EQ(trip.Join(false, 0xFFFFFFFF), (InvasionJoin{0x12345678, 4, false}));
    EXPECT_EQ(trip.Tick(At(31'000, Target)), InvaderTrip::Action::None);
    EXPECT_EQ(trip.Tick(At(32'000, Target, true, true)), InvaderTrip::Action::Place);
    EXPECT_EQ(trip.Current(), InvaderTrip::Phase::Visiting);
    EXPECT_EQ(trip.Tick(At(33'000, Target, true, true)), InvaderTrip::Action::None);
    trip.TakeEnd(End(7, bbcoop::InvasionEnd::TARGET_KILLED), 60'000);
    EXPECT_EQ(trip.Current(), InvaderTrip::Phase::GoingHome);
    EXPECT_FALSE(trip.TellsServer()); // the server ended it
    EXPECT_EQ(trip.EndReason(), bbcoop::InvasionEnd::TARGET_KILLED);
    EXPECT_EQ(trip.Tick(At(60'100, Target, true, true)), InvaderTrip::Action::GoHome);
    EXPECT_EQ(trip.Tick(At(60'200, Target, true, true)), InvaderTrip::Action::None);
    trip.Home();
    EXPECT_FALSE(trip.Active());
}

// Already on the target's map: it joins at once.
TEST(InvaderTrip, OnTheTargetsMapJoinsWithoutATrip) {
    InvaderTrip trip;
    trip.Ask(0);
    ASSERT_TRUE(trip.TakeStart(Start(1, Target), 100)); // a start with no notice before it
    EXPECT_EQ(trip.Tick(At(200, Target)), InvaderTrip::Action::Join);
}

// Its death in the target's world goes home without telling the server (its vitals do); a boss
// fog or the player leaving tell it.
TEST(InvaderTrip, DeathBossFogAndLeaving) {
    const auto visiting = [] {
        InvaderTrip trip;
        trip.Ask(0);
        trip.TakeStart(Start(1, Target), 0);
        trip.Tick(At(1, Target));
        trip.Tick(At(2, Target, true, true));
        return trip;
    };
    InvaderTrip died = visiting();
    InvaderTrip::Frame dead = At(3, Target, true, true);
    dead.dead = true;
    EXPECT_EQ(died.Tick(dead), InvaderTrip::Action::GoHome);
    EXPECT_TRUE(died.Died());
    EXPECT_FALSE(died.TellsServer());

    InvaderTrip fog = visiting();
    InvaderTrip::Frame boss = At(3, Target, true, true);
    boss.boss_fog = true;
    EXPECT_EQ(fog.Tick(boss), InvaderTrip::Action::GoHome);
    EXPECT_TRUE(fog.TellsServer());

    InvaderTrip left = visiting();
    EXPECT_TRUE(left.Cancel(3));
    EXPECT_TRUE(left.TellsServer());
    EXPECT_EQ(left.Tick(At(4, Target, true, true)), InvaderTrip::Action::GoHome);
    EXPECT_FALSE(left.Cancel(5)); // on its way home already
}

// A lost session (no word from the server either) sends it home after a while.
TEST(InvaderTrip, ASessionLostUnderItSendsItHome) {
    InvaderTrip trip;
    trip.Ask(0);
    trip.TakeStart(Start(1, Target), 0);
    trip.Tick(At(1, Target));
    trip.Tick(At(2, Target, true, true));
    EXPECT_EQ(trip.Tick(At(1'000, Target, true, false)), InvaderTrip::Action::None);
    EXPECT_EQ(trip.Tick(At(5'000, Target, true, true)), InvaderTrip::Action::None); // back
    EXPECT_EQ(trip.Tick(At(6'000, Target, true, false)), InvaderTrip::Action::None);
    EXPECT_EQ(trip.Tick(At(6'000 + InvaderTrip::SessionLostMs + 1, Target, true, false)),
              InvaderTrip::Action::GoHome);
}

// A join that never takes is tried again a few times, then the invader stays home.
TEST(InvaderTrip, AJoinThatNeverTakes) {
    InvaderTrip trip;
    trip.Ask(0);
    trip.TakeStart(Start(1, Target), 0);
    EXPECT_EQ(trip.Tick(At(1'000, Target)), InvaderTrip::Action::Join);
    EXPECT_EQ(trip.Tick(At(3'000, Target)), InvaderTrip::Action::None);
    EXPECT_EQ(trip.Tick(At(6'000, Target)), InvaderTrip::Action::Join);
    EXPECT_EQ(trip.Tick(At(11'000, Target)), InvaderTrip::Action::Join);
    EXPECT_EQ(trip.Tick(At(16'000, Target)), InvaderTrip::Action::None); // 3 tries
    EXPECT_EQ(trip.Tick(At(1'000 + InvaderTrip::JoinWaitMs + 1, Target)),
              InvaderTrip::Action::GoHome);
    EXPECT_TRUE(trip.TellsServer());
}

// Ends before the join: a refusal, nothing found, an end during the notice. Nothing to go home
// from.
TEST(InvaderTrip, EndsBeforeTheJoin) {
    InvaderTrip refused;
    refused.Ask(0);
    refused.TakeEnd(End(0, bbcoop::InvasionEnd_Reason_REFUSED), 10);
    EXPECT_FALSE(refused.Active());
    EXPECT_EQ(refused.EndReason(), bbcoop::InvasionEnd_Reason_REFUSED);

    InvaderTrip noticed;
    noticed.Ask(0);
    noticed.TakeNotice(Notice(3), 0);
    noticed.TakeEnd(End(3, bbcoop::InvasionEnd::HOST_LEFT), 10);
    EXPECT_FALSE(noticed.Active());
    EXPECT_FALSE(noticed.TakeStart(Start(3, Target), 20)); // too late

    InvaderTrip lost; // the server never answers
    lost.Ask(0);
    lost.Tick(At(InvaderTrip::SearchMs + 1, Home));
    EXPECT_FALSE(lost.Active());

    InvaderTrip idle; // no invasion: the server's messages change nothing
    idle.TakeNotice(Notice(1), 0);
    EXPECT_FALSE(idle.TakeStart(Start(1, Target), 0));
    idle.TakeEnd(End(1, bbcoop::InvasionEnd::TIMEOUT), 0);
    EXPECT_FALSE(idle.Active());
}

// The team's notice: approaching with its countdown, the invader in the world, the end shown a
// while.
TEST(InvadedTeam, NoticeJoinedEnd) {
    InvadedTeam team;
    EXPECT_EQ(InvasionViewOf({}, team, 0), InvasionView{});
    team.TakeNotice(Notice(5), 1'000);
    InvasionView view = InvasionViewOf({}, team, 1'500);
    EXPECT_EQ(view.role, InvasionView::Role::Target);
    EXPECT_EQ(view.phase, InvasionView::Phase::Approaching);
    EXPECT_EQ(view.seconds, 10);
    EXPECT_EQ(team.Invader(), ""); // not in the world yet
    bbcoop::InvaderJoined joined;
    joined.set_id(5);
    joined.set_member("inv");
    joined.set_name("Djura");
    joined.set_target("host");
    team.TakeJoined(joined, 11'000);
    EXPECT_EQ(team.Invader(), "inv");
    view = InvasionViewOf({}, team, 11'000);
    EXPECT_EQ(view.phase, InvasionView::Phase::Active);
    EXPECT_EQ(view.name, "Djura");
    team.TakeEnd(End(4, bbcoop::InvasionEnd::INVADER_DIED), 20'000); // another invasion's
    EXPECT_EQ(team.Current(), InvadedTeam::Phase::Invaded);
    team.TakeEnd(End(5, bbcoop::InvasionEnd::INVADER_DIED), 20'000);
    EXPECT_EQ(team.Invader(), "");
    view = InvasionViewOf({}, team, 21'000);
    EXPECT_EQ(view.phase, InvasionView::Phase::Ended);
    EXPECT_EQ(view.reason, bbcoop::InvasionEnd::INVADER_DIED);
    team.Tick(20'000 + InvadedTeam::EndShownMs);
    EXPECT_EQ(team.Current(), InvadedTeam::Phase::None);
}

TEST(InvadedTeam, TheInvadersOwnView) {
    InvaderTrip trip;
    trip.Ask(0);
    EXPECT_EQ(InvasionViewOf(trip, {}, 0).phase, InvasionView::Phase::Searching);
    trip.TakeNotice(Notice(1, 10'000), 0);
    const InvasionView view = InvasionViewOf(trip, {}, 2'500);
    EXPECT_EQ(view.role, InvasionView::Role::Invader);
    EXPECT_EQ(view.phase, InvasionView::Phase::Approaching);
    EXPECT_EQ(view.seconds, 8);
    // Nothing found: said a while, then nothing.
    trip.TakeEnd(End(1, bbcoop::InvasionEnd::NOT_FOUND), 3'000);
    EXPECT_EQ(InvasionViewOf(trip, {}, 4'000).phase, InvasionView::Phase::Ended);
    EXPECT_EQ(InvasionViewOf(trip, {}, 4'000).reason, bbcoop::InvasionEnd::NOT_FOUND);
    EXPECT_EQ(InvasionViewOf(trip, {}, 3'000 + InvadedTeam::EndShownMs).phase,
              InvasionView::Phase::None);
}

// Kill attribution: only a lethal check right after the invader's hit, never while down.
TEST(Invasion, TheInvadersKillIsARealDeath) {
    InvaderHitWindow window;
    EXPECT_FALSE(window.Fresh(0));
    window.Hit(1'000);
    EXPECT_TRUE(window.Fresh(1'500));
    EXPECT_FALSE(window.Fresh(1'000 + InvaderHitWindow::WindowMs));
    EXPECT_TRUE(InvaderKillGoesThrough(NoDeadCheck::Lethal, true, false));
    EXPECT_FALSE(InvaderKillGoesThrough(NoDeadCheck::Lethal, false, false));
    EXPECT_FALSE(InvaderKillGoesThrough(NoDeadCheck::Lethal, true, true));
    EXPECT_FALSE(InvaderKillGoesThrough(NoDeadCheck::AfterHit, true, false));
}

// DownedTeams never saves or gives back the invader's team; with the fallback it keeps the
// invaders' team on its body.
TEST(Invasion, DownedTeamsLeaveTheInvaderAlone) {
    DownedTeams teams;
    // The invader's body (7) is picked as downed by mistake: untouched.
    EXPECT_TRUE(teams.Tick({{7, 21}}, {{7, 21}}, std::vector<u32>{7}).empty());
    // A downed member (3) takes the invaders' team and gets its own back; the invader nothing.
    EXPECT_EQ(teams.Tick({{3, 20}}, {{3, 20}, {7, 21}}, std::vector<u32>{7}),
              (std::vector<CopyTeam>{{3, 21}}));
    EXPECT_EQ(teams.Tick({}, {{3, 21}, {7, 21}}, std::vector<u32>{7}),
              (std::vector<CopyTeam>{{3, 20}}));
    // The fallback: the invader's body back to the invaders' team whenever it reads otherwise.
    EXPECT_EQ(teams.Tick({}, {{7, 20}}, std::vector<u32>{7}, true),
              (std::vector<CopyTeam>{{7, 21}}));
    EXPECT_TRUE(teams.Tick({}, {{7, 20}}, std::vector<u32>{7}, false).empty());
}

// The invader's own world before the visit: its file, and every flag put back (not only the
// world's).
TEST(Invasion, HomeSnapshotRoundTrip) {
    InvasionHome home;
    home.respawn = 2100;
    home.flags.blocks = {0, 1};
    home.flags.on = {9180, 12};
    std::sort(home.flags.on.begin(), home.flags.on.end());
    const auto parsed = ParseInvasionHome(FormatInvasionHome(home));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, home);
    EXPECT_FALSE(ParseInvasionHome("").has_value());
    EXPECT_FALSE(ParseInvasionHome("respawn x\nblocks\n").has_value());
}

TEST(Invasion, HomeFlagsComeBackWhole) {
    // One block of 16 flags: flags 0..15, MSB first. The visit set 3 and cleared 9.
    const std::array<u8, 2> before{0x00, 0x40}; // 9 on
    const std::array<u8, 2> after{0x10, 0x00};  // 3 on
    const FlagBlockBits was{0, before};
    const WorldFlags home = CaptureWorldFlags(16, std::span<const FlagBlockBits>{&was, 1}, true);
    EXPECT_EQ(home.on, std::vector<s32>{9});
    const FlagBlockBits now{0, after};
    const auto changes = WorldFlagChanges(16, std::span<const FlagBlockBits>{&now, 1}, home, true);
    EXPECT_EQ(changes, (std::vector<std::pair<s32, bool>>{{3, false}, {9, true}}));
}
