// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "core/libraries/network/net_link.h"

using namespace Libraries::Net;
using namespace Libraries::Net::Link;

namespace {

// server/tests/contracts/server_link_frames.txt: the datagrams shadNet's tests check too.
std::map<std::string, std::vector<u8>> ContractDatagrams() {
    std::map<std::string, std::vector<u8>> datagrams;
    std::ifstream file{BLOODBORNE_SERVER_LINK_CONTRACTS};
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        std::vector<u8> bytes;
        for (size_t at = tab + 1; at + 1 < line.size(); at += 2) {
            bytes.push_back(static_cast<u8>(std::stoul(line.substr(at, 2), nullptr, 16)));
        }
        datagrams[line.substr(0, tab)] = std::move(bytes);
    }
    return datagrams;
}

constexpr u32 Loopback = 0x0100007F; // 127.0.0.1 as sockaddr_in holds it on a little-endian PC
constexpr u16 Port1234 = 0x3412;     // port 0x1234 in network byte order

const std::vector<u8> GameFrame{0x75, 0x30, 0x75, 0x31, 0x07, 0x00, 0x00,
                                0x00, 0x02, 0x00, 0x00, 0x00, 0xab, 0xcd};

} // namespace

TEST(NetLink, EncodesTheContractsDatagrams) {
    const auto datagrams = ContractDatagrams();
    ASSERT_EQ(datagrams.size(), 12u) << BLOODBORNE_SERVER_LINK_CONTRACTS;
    Hello hello{
        .online_id = "Kaniel", .build = "92829ac4 events 1f protocol 9", .secret = "s3cr3t"};
    hello.cookie.fill(0x11);
    hello.nonce.fill(0x22);
    EXPECT_EQ(Encode(hello), datagrams.at("hello"));
    EXPECT_EQ(EncodeCookie(hello.cookie), datagrams.at("cookie"));
    const std::vector<u8> rpc_body{0x01, 0x02};
    EXPECT_EQ(EncodeRpc(rpc_body), datagrams.at("rpc"));
    EXPECT_EQ(Encode(Welcome{.accepted = true}), datagrams.at("welcome.ok"));
    EXPECT_EQ(Encode(Welcome{.accepted = false, .text = "another build: 13ddd14d"}),
              datagrams.at("welcome.refused"));
    EXPECT_EQ(EncodeData(Loopback, Port1234, GameFrame), datagrams.at("data"));
    EXPECT_EQ(Encode(Kind::Ping, Ping{7, 1000}), datagrams.at("ping"));
    EXPECT_EQ(Encode(Kind::Pong, Ping{7, 1000}), datagrams.at("pong"));
    EXPECT_EQ(Encode(Member{MemberState::Quiet, 3500, Loopback, Port1234, "Ada"}),
              datagrams.at("member.quiet"));
    EXPECT_EQ(Encode(Member{MemberState::Back, 4200, Loopback, Port1234, "Ada"}),
              datagrams.at("member.back"));
    EXPECT_EQ(Encode(Member{MemberState::Gone, 60000, Loopback, Port1234, "Ada"}),
              datagrams.at("member.gone"));
}

TEST(NetLink, DecodesTheContractsDatagrams) {
    const auto datagrams = ContractDatagrams();
    ASSERT_FALSE(datagrams.empty());
    const auto hello = DecodeHello(datagrams.at("hello"));
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->version, LinkVersion);
    EXPECT_EQ(hello->online_id, "Kaniel");
    EXPECT_EQ(hello->build, "92829ac4 events 1f protocol 9");
    EXPECT_EQ(hello->version, 3) << "link version 3: the cookie and the tags";
    EXPECT_EQ(hello->secret, "s3cr3t");
    EXPECT_EQ(hello->cookie[15], 0x11);
    EXPECT_EQ(hello->nonce[0], 0x22);
    const auto cookie = DecodeCookie(datagrams.at("cookie"));
    ASSERT_TRUE(cookie.has_value());
    EXPECT_EQ((*cookie)[0], 0x11);
    const auto rpc = DecodeRpc(datagrams.at("rpc"));
    ASSERT_TRUE(rpc.has_value());
    EXPECT_EQ(std::vector<u8>(rpc->begin(), rpc->end()), (std::vector<u8>{0x01, 0x02}));
    EXPECT_EQ(KindOf(datagrams.at("rpc")), Kind::Rpc);
    EXPECT_FALSE(DecodeCoop(datagrams.at("rpc")).has_value());

    const auto refused = DecodeWelcome(datagrams.at("welcome.refused"));
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(refused->accepted);
    EXPECT_EQ(refused->text, "another build: 13ddd14d");
    EXPECT_TRUE(DecodeWelcome(datagrams.at("welcome.ok"))->accepted);

    const auto data = DecodeData(datagrams.at("data"));
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->addr_nbo, Loopback);
    EXPECT_EQ(data->port_nbo, Port1234);
    EXPECT_EQ(std::vector<u8>(data->frame.begin(), data->frame.end()), GameFrame);

    const auto pong = DecodePing(Kind::Pong, datagrams.at("pong"));
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong->seq, 7u);
    EXPECT_EQ(pong->sent_ms, 1000u);
    EXPECT_FALSE(DecodePing(Kind::Ping, datagrams.at("pong")).has_value()) << "a pong is no ping";

    const auto quiet = DecodeMember(datagrams.at("member.quiet"));
    ASSERT_TRUE(quiet.has_value());
    EXPECT_EQ(quiet->state, MemberState::Quiet);
    EXPECT_EQ(quiet->gap_ms, 3500u);
    EXPECT_EQ(quiet->addr_nbo, Loopback);
    EXPECT_EQ(quiet->port_nbo, Port1234);
    EXPECT_EQ(quiet->online_id, "Ada");
    EXPECT_EQ(DecodeMember(datagrams.at("member.gone"))->state, MemberState::Gone);
}

TEST(NetLink, EveryCutDatagramIsRefused) {
    for (const auto& [name, bytes] : ContractDatagrams()) {
        for (size_t size = 0; size < bytes.size(); ++size) {
            const std::span<const u8> cut{bytes.data(), size};
            if (name == "data" && size >= 8 + 1 + 6) {
                continue; // a shorter frame is still a frame
            }
            if (name == "ping.tagged" && size >= 9 + 12) {
                continue; // a ping with bytes after it
            }
            EXPECT_FALSE(DecodeHello(cut) || DecodeWelcome(cut) || DecodeData(cut) ||
                         DecodeCookie(cut) || DecodePing(Kind::Ping, cut) ||
                         DecodePing(Kind::Pong, cut) || DecodeMember(cut))
                << name << " cut to " << size;
        }
    }
}

TEST(NetLink, TellsLinkDatagramsFromSignaling) {
    const auto datagrams = ContractDatagrams();
    EXPECT_EQ(KindOf(datagrams.at("data")), Kind::Data);
    EXPECT_EQ(KindOf(datagrams.at("member.back")), Kind::Member);
    // The STUN ping and the signaling frames share the vport 0xffff header.
    const std::vector<u8> stun{0xff, 0xff, 0xff, 0xff, 0x01, 'K', 'a', 'n', 'i', 'e', 'l'};
    EXPECT_FALSE(KindOf(stun).has_value());
    const std::vector<u8> shad{0xff, 0xff, 0xff, 0xff, 'S', 'H', 'A', 'D', 0x33};
    EXPECT_FALSE(KindOf(shad).has_value());
    const std::vector<u8> unknown{0xff, 0xff, 0xff, 0xff, 'S', 'H', 'L', 'K', 0x7f};
    EXPECT_FALSE(KindOf(unknown).has_value());
}

// Every frame goes to the server but what is for the server itself (the link's own kinds).
TEST(NetLink, RoutesEveryPeerThroughTheServer) {
    const ServerEndpoint server{Loopback, 0x2A7A};
    EXPECT_TRUE(RoutesThroughServer(server, 0x0201A8C0, Port1234));
    EXPECT_TRUE(RoutesThroughServer(server, Loopback, Port1234));
    EXPECT_FALSE(RoutesThroughServer(server, Loopback, 0x2A7A)) << "the server's own port";
    EXPECT_FALSE(RoutesThroughServer(ServerEndpoint{}, Loopback, Port1234)) << "no server yet";
}

// Phase 9: a peer's datagrams only ever travel through the server, never straight between
// games, in every build; before the link nothing goes and nothing comes in.
TEST(NetLink, AGameTalksToPeersOnlyThroughTheServer) {
    const ServerEndpoint server{Loopback, 0x2A7A};
    EXPECT_TRUE(SendsToPeer(server)) << "relayed by the server";
    EXPECT_FALSE(SendsToPeer(ServerEndpoint{})) << "no link yet";
    EXPECT_TRUE(TakesDatagramFrom(server, Loopback, 0x2A7A));
    EXPECT_FALSE(TakesDatagramFrom(server, 0x0201A8C0, Port1234)) << "straight from a peer";
    EXPECT_FALSE(TakesDatagramFrom(ServerEndpoint{}, Loopback, 0x2A7A)) << "no link yet";
}

// Phase 7: the peers are the server's room members at their virtual addresses, all reached
// through the server: a member is connected at once, with no handshake and no STUN.
// Phase 11: the link starts with the NP sign-in, before the co-op code says the game's build; a
// hello before that would leave shadNet an empty build to hold every later hello against.
TEST(NetLink, AHelloGoesAgainOnceTheBuildIsKnown) {
    EXPECT_TRUE(HelloDue(false, std::nullopt, false, 0));
    EXPECT_FALSE(HelloDue(true, std::nullopt, false, 0)) << "welcomed: none";
    EXPECT_FALSE(HelloDue(false, 0, false, 999));
    EXPECT_TRUE(HelloDue(false, 0, false, 1000)) << "once a second";
    EXPECT_FALSE(HelloDue(false, 0, true, 9999));
    EXPECT_TRUE(HelloDue(false, 0, true, 10000)) << "refused: every 10 s";
    // Welcomed before the co-op code said the build: a hello again with it.
    EXPECT_TRUE(SaysHelloAgainForBuild("", "abc events 1 protocol 9"));
    EXPECT_FALSE(SaysHelloAgainForBuild("abc", "abc"));
    EXPECT_FALSE(SaysHelloAgainForBuild("", ""));
}

TEST(NetLink, PeersNeedNoHandshake) {
    EXPECT_TRUE(ConnectsWithoutHandshake(ServerEndpoint{Loopback, 0x2A7A}));
    EXPECT_FALSE(ConnectsWithoutHandshake(ServerEndpoint{})) << "no link yet";
}

// The game's own address, as the game's NAT queries see it, is its virtual one (the server's
// VirtualEndpointOf, server/tests/test_server_link.cpp): no STUN, and never its real one.
TEST(NetLink, AGameIsKnownByTheVirtualAddressTheServerGivesIt) {
    EXPECT_EQ(VirtualAddressOf("Kaniel").ipv4, 0x0A89C805u);
    EXPECT_EQ(VirtualAddressOf("Ada").ipv4, 0x0AC3A55Bu);
    EXPECT_EQ(VirtualAddressOf("Ada").port, 3658);
}

TEST(NetLink, PingsOnceASecondAndMeasuresTheRoundTrip) {
    PingMeter meter;
    const auto first = meter.Due(0);
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(meter.Due(999).has_value());
    const auto second = meter.Due(1000);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->seq, first->seq + 1);
    EXPECT_EQ(second->sent_ms, 1000u);
    EXPECT_FALSE(meter.Pong(*first, 80).has_value()) << "no outage to report";
    meter.Pong(*second, 1120);
    const auto summary = meter.TakeSummary(10'000);
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->pongs, 2u);
    EXPECT_EQ(summary->min_ms, 80);
    EXPECT_EQ(summary->max_ms, 120);
    EXPECT_EQ(summary->avg_ms, 100);
    EXPECT_FALSE(meter.TakeSummary(12'000).has_value()) << "every 10 s";
}

TEST(NetLink, AServerThatStopsAnsweringIsAnOutageReportedOnce) {
    PingMeter meter;
    meter.Pong(*meter.Due(0), 50);
    for (s64 now = 1000; now <= 3000; now += 1000) {
        meter.Due(now);
        EXPECT_FALSE(meter.Check(now).has_value()) << now;
    }
    meter.Due(4000);
    const auto quiet = meter.Check(4000);
    ASSERT_TRUE(quiet.has_value());
    EXPECT_TRUE(quiet->quiet);
    EXPECT_EQ(quiet->gap_ms, 3950);
    meter.Due(5000);
    EXPECT_FALSE(meter.Check(5000).has_value()) << "once";
    const auto back = meter.Pong(Ping{5, 5000}, 5100);
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(back->quiet);
    EXPECT_EQ(back->gap_ms, 5050);
}

TEST(NetLink, NoAnswerAtAllIsAnOutageToo) {
    PingMeter meter;
    meter.Due(0);
    EXPECT_FALSE(meter.Check(3000).has_value());
    meter.Due(3000);
    const auto quiet = meter.Check(3001);
    ASSERT_TRUE(quiet.has_value());
    EXPECT_TRUE(quiet->quiet);
}

// Link version 3 (server/coop/link_auth.h): the game's side of the handshake and the tags, as
// shadNet's tests check the server's (server/tests/test_server_link.cpp).
TEST(NetLink, TheFirstHelloAsksForACookieAndCarriesNoSecret) {
    LinkAuth::Client auth;
    LinkAuth::Nonce nonce{};
    nonce.fill(0x22);
    auth.Restart(nonce);
    const Hello first = HelloFor(auth, "Kaniel", "b", "s3cr3t");
    EXPECT_TRUE(first.secret.empty());
    EXPECT_EQ(first.cookie, LinkAuth::Cookie{});
    EXPECT_FALSE(Sealed(auth, Encode(Kind::Ping, Ping{1, 1})).has_value()) << "no session yet";
    LinkAuth::Cookie cookie{};
    cookie.fill(0x11);
    auth.TakeCookie(cookie);
    const Hello second = HelloFor(auth, "Kaniel", "92829ac4 events 1f protocol 9", "s3cr3t");
    EXPECT_EQ(second.secret, "s3cr3t");
    EXPECT_EQ(Encode(second), ContractDatagrams().at("hello"));
    // The game's first datagram of that link: what shadNet's tests expect byte for byte.
    EXPECT_EQ(*Sealed(auth, Encode(Kind::Ping, Ping{7, 1000})),
              ContractDatagrams().at("ping.tagged"));
}

TEST(NetLink, OnlyTheServersTaggedDatagramsOpen) {
    LinkAuth::Nonce nonce{};
    nonce[0] = 5;
    LinkAuth::Cookie cookie{};
    cookie[0] = 7;
    LinkAuth::Client auth;
    auth.Restart(nonce);
    auth.TakeCookie(cookie);
    HelloFor(auth, "Ada", "b", "pw");
    LinkAuth::Session server =
        LinkAuth::ServerSession(LinkAuth::KeysFor("pw", nonce, cookie, "Ada"));
    const auto seal = [&](std::vector<u8> datagram) {
        const auto trailer = server.Seal(datagram);
        datagram.insert(datagram.end(), trailer.begin(), trailer.end());
        return datagram;
    };
    const auto welcome = seal(Encode(Welcome{.accepted = true}));
    const auto opened = Opened(auth, welcome);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(DecodeWelcome(*opened)->accepted);
    EXPECT_FALSE(Opened(auth, welcome).has_value()) << "a replay";

    const std::vector<u8> body{1, 2, 3};
    auto coop = seal(EncodeCoop(body));
    auto forged = coop;
    forged[10] ^= 1;
    EXPECT_FALSE(Opened(auth, forged).has_value()) << "a bad tag";
    EXPECT_FALSE(Opened(auth, EncodeCoop(body)).has_value()) << "no tag (a spoofed source)";
    const auto plain = Opened(auth, coop);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(DecodeCoop(*plain)->size(), 3u);

    // The game's datagrams open on the server's side with the other key.
    const auto up = Sealed(auth, EncodeRpc(body));
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(server.Open(*up), LinkAuth::Session::Verdict::Ok);
    EXPECT_EQ(server.Open(*up), LinkAuth::Session::Verdict::Replay);
    EXPECT_FALSE(Opened(auth, *up).has_value()) << "its own datagram reflected back";

    // A hello again with the same nonce and cookie keeps the session (and its counters); a new
    // nonce (the link restarted) makes another, the old one in use until the server welcomes it.
    HelloFor(auth, "Ada", "b", "pw");
    EXPECT_TRUE(Opened(auth, seal(EncodeCoop(body))).has_value());
    LinkAuth::Nonce next = nonce;
    next[1] = 1;
    auth.Restart(next);
    HelloFor(auth, "Ada", "b", "pw");
    EXPECT_TRUE(Opened(auth, seal(EncodeCoop(body))).has_value()) << "the old session still";
    LinkAuth::Session renewed =
        LinkAuth::ServerSession(LinkAuth::KeysFor("pw", next, cookie, "Ada"));
    auto welcome_again = Encode(Welcome{.accepted = true});
    const auto trailer = renewed.Seal(welcome_again);
    welcome_again.insert(welcome_again.end(), trailer.begin(), trailer.end());
    EXPECT_TRUE(Opened(auth, welcome_again).has_value());
    EXPECT_FALSE(Opened(auth, seal(EncodeCoop(body))).has_value()) << "the old one is over";
}
