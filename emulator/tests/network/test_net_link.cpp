// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
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

// The contracts' server key: its secret 32 x 0x44.
LinkAuth::ServerKey ServerKey() {
    LinkAuth::Key secret;
    secret.fill(0x44);
    return LinkAuth::ServerKeyOf(secret);
}

LinkAuth::Key Filled(u8 byte) {
    LinkAuth::Key key;
    key.fill(byte);
    return key;
}

// A new ephemeral key for each handshake (past byte 0, whose low bits X25519 clears).
LinkAuth::Key NextKey() {
    static u32 count = 0;
    LinkAuth::Key key = Filled(0x60);
    ++count;
    std::memcpy(key.data() + 1, &count, sizeof(count));
    return key;
}

constexpr u32 Loopback = 0x0100007F; // 127.0.0.1 as sockaddr_in holds it on a little-endian PC
constexpr u16 Port1234 = 0x3412;     // port 0x1234 in network byte order

const std::vector<u8> GameFrame{0x75, 0x30, 0x75, 0x31, 0x07, 0x00, 0x00,
                                0x00, 0x02, 0x00, 0x00, 0x00, 0xab, 0xcd};

} // namespace

TEST(NetLink, EncodesTheContractsDatagrams) {
    const auto datagrams = ContractDatagrams();
    ASSERT_EQ(datagrams.size(), 17u) << BLOODBORNE_SERVER_LINK_CONTRACTS;
    Hello hello{
        .online_id = "Kaniel", .build = "92829ac4 events 1f protocol 9", .secret = "s3cr3t"};
    hello.cookie.fill(0x11);
    EXPECT_EQ(Encode(hello), datagrams.at("hello"));
    EXPECT_EQ(EncodeCookie(hello.cookie, 20, ServerKey().pub), datagrams.at("cookie"));
    EXPECT_EQ(EncodeWork(20), datagrams.at("work"));
    hello.work = 0x0807060504030201;
    EXPECT_EQ(Encode(hello), datagrams.at("hello.work"));
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
    EXPECT_EQ(hello->version, 4) << "link version 4: the handshake and the encryption";
    EXPECT_EQ(hello->secret, "s3cr3t");
    EXPECT_EQ(hello->cookie[15], 0x11);
    EXPECT_FALSE(hello->work.has_value());
    int work_bits = 0;
    LinkAuth::Key key{};
    const auto cookie = DecodeCookie(datagrams.at("cookie"), &work_bits, &key);
    ASSERT_TRUE(cookie.has_value());
    EXPECT_EQ((*cookie)[0], 0x11);
    EXPECT_EQ(work_bits, 20);
    EXPECT_EQ(key, ServerKey().pub);
    EXPECT_EQ(std::vector<u8>(key.begin(), key.end()), datagrams.at("server.key"));
    EXPECT_EQ(DecodeHello(datagrams.at("hello.work"))->work, 0x0807060504030201u);
    EXPECT_EQ(DecodeWork(datagrams.at("work")), 20);
    EXPECT_EQ(KindOf(datagrams.at("work")), Kind::Work);
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
        if (name == "server.key" || name.ends_with(".sealed")) {
            continue; // a key; datagrams read only once opened
        }
        for (size_t size = 0; size < bytes.size(); ++size) {
            const std::span<const u8> cut{bytes.data(), size};
            if (name == "data" && size >= 8 + 1 + 6) {
                continue; // a shorter frame is still a frame
            }
            if (name == "hello.work" && size >= ContractDatagrams().at("hello").size()) {
                continue; // without the work
            }
            EXPECT_FALSE(DecodeHello(cut) || DecodeWelcome(cut) || DecodeData(cut) ||
                         DecodeCookie(cut) || DecodePing(Kind::Ping, cut) ||
                         DecodePing(Kind::Pong, cut) || DecodeMember(cut) || DecodeWork(cut))
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

// Link version 4 (server/coop/link_auth.h): the game's side of the handshake and the encryption,
// as shadNet's tests check the server's (server/tests/test_server_link.cpp); LinkAuth::Responder
// plays the server.
TEST(NetLink, TheFirstHelloAsksForACookieAndCarriesNoSecret) {
    const auto contracts = ContractDatagrams();
    LinkAuth::Client auth;
    const auto first = HelloDatagram(auth, "Kaniel", "b", "s3cr3t", NextKey());
    EXPECT_EQ(first.size(), LinkAuth::FirstHelloBytes);
    EXPECT_EQ(std::search(first.begin(), first.end(), std::begin("s3cr3t"), std::end("s3cr3t") - 1),
              first.end());
    EXPECT_EQ(DecodeHello(first)->cookie, LinkAuth::Cookie{}) << "the cookie asked for";
    EXPECT_FALSE(Sealed(auth, Encode(Kind::Ping, Ping{1, 1})).has_value()) << "no session yet";
    LinkAuth::Cookie cookie{};
    cookie.fill(0x11);
    ASSERT_TRUE(auth.TakeCookie(cookie, ServerKey().pub));
    // The hello sealed to the server's key: the bytes shadNet's tests and noise_link.py expect.
    const auto second =
        HelloDatagram(auth, "Kaniel", "92829ac4 events 1f protocol 9", "s3cr3t", Filled(0x33));
    EXPECT_EQ(second, contracts.at("hello.sealed"));
    EXPECT_EQ(HelloDatagram(auth, "Kaniel", "92829ac4 events 1f protocol 9", "s3cr3t", NextKey()),
              second)
        << "said again: the same datagram";
    std::vector<u8> plain;
    ASSERT_TRUE(LinkAuth::Responder::Open(ServerKey(), second, plain).has_value());
    EXPECT_EQ(plain, contracts.at("hello"));
    // Its welcome opens, and the game's first datagram of that link is the contracts'.
    const auto welcome = Opened(auth, contracts.at("welcome.sealed"));
    ASSERT_TRUE(welcome.has_value());
    EXPECT_TRUE(DecodeWelcome(*welcome)->accepted);
    EXPECT_EQ(*Sealed(auth, Encode(Kind::Ping, Ping{7, 1000})), contracts.at("ping.sealed"));
}

TEST(NetLink, OnlyTheServersSealedDatagramsOpen) {
    LinkAuth::Cookie cookie{};
    cookie[0] = 7;
    LinkAuth::Client auth;
    auth.Pin(ServerKey().pub);
    ASSERT_TRUE(auth.TakeCookie(cookie, ServerKey().pub));
    const auto hello = HelloDatagram(auth, "Ada", "b", "pw", NextKey());
    std::vector<u8> plain_hello;
    const auto server_hello = LinkAuth::Responder::Open(ServerKey(), hello, plain_hello);
    ASSERT_TRUE(server_hello.has_value());
    EXPECT_EQ(DecodeHello(plain_hello)->secret, "pw");
    auto answered = server_hello->Answer(Encode(Welcome{.accepted = true}), NextKey());
    LinkAuth::Session& server = answered.session;
    const auto seal = [&](std::vector<u8> datagram) {
        const auto trailer = server.Seal(datagram);
        datagram.insert(datagram.end(), trailer.begin(), trailer.end());
        return datagram;
    };
    // A welcome changed on the way, or answering another game's hello: not this game's.
    auto changed = answered.datagram;
    changed[30] ^= 1;
    EXPECT_FALSE(Opened(auth, changed).has_value());
    LinkAuth::Client other;
    other.TakeCookie(cookie, ServerKey().pub);
    std::vector<u8> unused;
    const auto other_answer =
        LinkAuth::Responder::Open(ServerKey(), HelloDatagram(other, "Ada", "b", "pw", NextKey()),
                                  unused)
            ->Answer(Encode(Welcome{.accepted = true}), NextKey());
    EXPECT_FALSE(Opened(auth, other_answer.datagram).has_value());
    EXPECT_FALSE(auth.Linked());
    const auto opened = Opened(auth, answered.datagram);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(DecodeWelcome(*opened)->accepted);
    EXPECT_TRUE(auth.Linked());
    EXPECT_FALSE(Opened(auth, answered.datagram).has_value()) << "its handshake is done";

    const std::vector<u8> body{1, 2, 3};
    auto coop = seal(EncodeCoop(body));
    EXPECT_NE(std::vector<u8>(coop.begin() + 9, coop.begin() + 12), body) << "encrypted";
    auto forged = coop;
    forged[10] ^= 1;
    EXPECT_FALSE(Opened(auth, forged).has_value()) << "changed on the way";
    EXPECT_FALSE(Opened(auth, EncodeCoop(body)).has_value()) << "not sealed (a spoofed source)";
    const auto plain = Opened(auth, coop);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(DecodeCoop(*plain)->size(), 3u);
    EXPECT_FALSE(Opened(auth, coop).has_value()) << "a replay";

    // The game's datagrams open on the server's side with the other key.
    auto up = Sealed(auth, EncodeRpc(body));
    ASSERT_TRUE(up.has_value());
    auto reflected = *up;
    EXPECT_EQ(server.Open(*up), LinkAuth::Session::Verdict::Ok);
    auto again = Sealed(auth, EncodeRpc(body));
    auto replayed = *again;
    EXPECT_EQ(server.Open(*again), LinkAuth::Session::Verdict::Ok);
    EXPECT_EQ(server.Open(replayed), LinkAuth::Session::Verdict::Replay);
    EXPECT_FALSE(Opened(auth, reflected).has_value()) << "its own datagram reflected back";

    // The link restarted (the server may have restarted): a new handshake, the old session in
    // use until the server welcomes it.
    auth.Restart();
    const auto renewed_hello = HelloDatagram(auth, "Ada", "b", "pw", NextKey());
    EXPECT_NE(renewed_hello, hello);
    EXPECT_TRUE(Opened(auth, seal(EncodeCoop(body))).has_value()) << "the old session still";
    const auto renewed = LinkAuth::Responder::Open(ServerKey(), renewed_hello, unused)
                             ->Answer(Encode(Welcome{.accepted = true}), NextKey());
    EXPECT_TRUE(Opened(auth, renewed.datagram).has_value());
    EXPECT_FALSE(Opened(auth, seal(EncodeCoop(body))).has_value()) << "the old one is over";
}

// The server's key: the launcher's, or the first cookie's once a welcome proves it.
TEST(NetLink, TheServersKeyIsPinnedOrTrustedOnFirstUse) {
    LinkAuth::Cookie cookie{};
    cookie[0] = 9;
    const auto impostor = LinkAuth::ServerKeyOf(Filled(0x77));
    LinkAuth::Client pinned;
    pinned.Pin(ServerKey().pub);
    EXPECT_FALSE(pinned.TakeCookie(cookie, impostor.pub)) << "another key than the launcher's";
    EXPECT_FALSE(pinned.HasCookie());

    // No key from the launcher: the cookie's, kept only once its welcome opened.
    LinkAuth::Client home;
    ASSERT_TRUE(home.TakeCookie(cookie, impostor.pub)); // a forged cookie came first
    EXPECT_FALSE(home.Pinned());
    ASSERT_TRUE(home.TakeCookie(cookie, ServerKey().pub)); // then the server's
    const auto hello = HelloDatagram(home, "Eileen", "b", "pw", NextKey());
    std::vector<u8> unused;
    EXPECT_FALSE(LinkAuth::Responder::Open(impostor, hello, unused).has_value());
    const auto welcome = LinkAuth::Responder::Open(ServerKey(), hello, unused)
                             ->Answer(Encode(Welcome{.accepted = true}), NextKey());
    ASSERT_TRUE(Opened(home, welcome.datagram).has_value());
    EXPECT_TRUE(home.Pinned());
    EXPECT_EQ(home.TakeLearned(), ServerKey().pub) << "said once, for the launcher to keep";
    EXPECT_FALSE(home.TakeLearned().has_value());
    EXPECT_FALSE(home.TakeCookie(cookie, impostor.pub));
    EXPECT_EQ(LinkAuth::KeyFromHex(LinkAuth::HexOf(ServerKey().pub)), ServerKey().pub);
    EXPECT_FALSE(LinkAuth::KeyFromHex("not a key").has_value());
}

// A hosted server's first sign-in: the server's sealed Work answer, the work done for the cookie,
// and the hello that carries it (a new handshake: its bytes changed).
TEST(NetLink, AFirstSignInDoesTheWorkTheServerAsks) {
    LinkAuth::Cookie cookie{};
    cookie[0] = 7;
    LinkAuth::Client auth;
    auth.TakeCookie(cookie, ServerKey().pub);
    const auto hello = HelloDatagram(auth, "Ada", "b", "pw", NextKey());
    std::vector<u8> plain;
    const auto server = LinkAuth::Responder::Open(ServerKey(), hello, plain);
    ASSERT_TRUE(server.has_value());
    EXPECT_FALSE(DecodeHello(plain)->work.has_value());
    const auto opened = Opened(auth, server->Answer(EncodeWork(12), NextKey()).datagram);
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(DecodeWork(*opened), 12);
    EXPECT_FALSE(auth.Linked());
    EXPECT_FALSE(Opened(auth, EncodeWork(30)).has_value()) << "not sealed: nobody's to obey";

    ASSERT_TRUE(auth.WorkAsked(12));
    EXPECT_TRUE(auth.Working());
    EXPECT_FALSE(auth.WorkAsked(12)) << "asked again while under way";
    const auto work = LinkAuth::SolveWork(cookie, "Ada", 12, 0, u64{1} << 24);
    ASSERT_TRUE(work.has_value());
    auth.TakeWork(cookie, 12, work);
    EXPECT_FALSE(auth.Working());
    const auto worked = HelloDatagram(auth, "Ada", "b", "pw", NextKey());
    EXPECT_NE(worked, hello);
    const auto server_again = LinkAuth::Responder::Open(ServerKey(), worked, plain);
    ASSERT_TRUE(server_again.has_value());
    const auto said = DecodeHello(plain);
    ASSERT_TRUE(said->work.has_value());
    EXPECT_TRUE(LinkAuth::WorkDone(cookie, "Ada", *said->work, 12));
    // Its welcome opens.
    EXPECT_TRUE(
        Opened(auth, server_again->Answer(Encode(Welcome{.accepted = true}), NextKey()).datagram)
            .has_value());
    EXPECT_TRUE(auth.Linked());
    // A new cookie (the old one ran out): the work is for the old one only.
    LinkAuth::Cookie later = cookie;
    later[1] = 1;
    auth.TakeCookie(later, ServerKey().pub);
    LinkAuth::Responder::Open(ServerKey(), HelloDatagram(auth, "Ada", "b", "pw", NextKey()), plain);
    EXPECT_FALSE(DecodeHello(plain)->work.has_value());
    EXPECT_TRUE(auth.WorkAsked(12)) << "asked again for the new cookie";
}
