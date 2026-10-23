// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The server link's datagrams as the emulator writes them
// (tests/contracts/server_link_frames.txt, which the emulator's tests read
// too), and the hub that forwards the games' frames between them.
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <QMessageAuthenticationCode>

#include "coop/link_auth.h"
#include "coop/packet_routes.h"
#include "server_link.h"

using namespace ServerLink;

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

std::map<std::string, QByteArray> ReadContracts(const char *path) {
  std::map<std::string, QByteArray> datagrams;
  std::ifstream file(path);
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
    datagrams[line.substr(0, tab)] =
        QByteArray::fromHex(QByteArray::fromStdString(line.substr(tab + 1)));
  }
  return datagrams;
}

const Endpoint Ada127{0x7F000001, 0x1234};
const QByteArray GameFrame = QByteArray::fromHex("75307531070000000200000"
                                                 "0abcd");
const QByteArray Build = "92829ac4 events 1f protocol 9";

const Endpoint Kaniel{0x7F000001, 40001};
const Endpoint Ada{0x7F000001, 40002};
const Endpoint Eileen{0xC0A80105, 40003};

QByteArray HelloFrom(const char *who, const QByteArray &build = Build,
                     const QByteArray &secret = "s3cr3t") {
  return Encode(Hello{LinkVersion, who, build, secret, {}, {}});
}

QByteArray Bytes(const LinkAuth::Trailer &trailer) {
  return QByteArray(reinterpret_cast<const char *>(trailer.data()),
                    static_cast<qsizetype>(trailer.size()));
}

std::span<const u8> Span(const QByteArray &bytes) {
  return {reinterpret_cast<const u8 *>(bytes.constData()),
          static_cast<size_t>(bytes.size())};
}

// The sends of `result` to `to`.
QList<QByteArray> SentTo(const Hub::Result &result, const Endpoint &to) {
  QList<QByteArray> sent;
  for (const auto &send : result.sends) {
    if (send.to == to) {
      sent.append(send.datagram);
    }
  }
  return sent;
}

// The hub with the games' side of the link around it (coop/link_auth.h, as the
// emulator runs it): a hello goes through the cookie round trip, a game's
// datagram goes tagged, and the hub's answers come back opened by their game
// (one that does not open is dropped and counted in `unopened`).
class Net {
public:
  template <typename... Args> explicit Net(Args... args) : hub(args...) {}

  Hub::Result Receive(const Endpoint &from, const QByteArray &datagram,
                      qint64 now_ms) {
    const auto hello =
        KindOf(datagram) == Kind::Hello ? DecodeHello(datagram) : std::nullopt;
    if (!hello || hello->version != LinkVersion) {
      QByteArray sent = datagram;
      if (const auto found = games.find(from.Key()); found != games.end()) {
        if (const auto trailer = found->second.Seal(Span(sent))) {
          sent += Bytes(*trailer);
        }
      }
      return Opened(hub.Receive(from, sent, now_ms));
    }
    auto [game, made] = games.try_emplace(from.Key());
    if (made) {
      LinkAuth::Nonce nonce{};
      nonce[0] = static_cast<u8>(++nonces);
      game->second.Restart(nonce);
    }
    for (int round = 0; round < 3; ++round) {
      Hello said = *hello;
      if (game->second.HasCookie()) {
        game->second.ForHello(said.secret.toStdString(),
                              said.online_id.toStdString());
        const auto &cookie = game->second.CookieToEcho();
        said.cookie = QByteArray(reinterpret_cast<const char *>(cookie.data()),
                                 LinkAuth::CookieBytes);
      } else {
        said.secret.clear(); // only once the cookie came
      }
      const auto &nonce = game->second.NonceToSend();
      said.nonce = QByteArray(reinterpret_cast<const char *>(nonce.data()),
                              LinkAuth::NonceBytes);
      Hub::Result result = hub.Receive(from, Encode(said), now_ms);
      const auto sent = SentTo(result, from);
      const auto cookie =
          sent.size() == 1 ? DecodeCookie(sent[0]) : std::nullopt;
      if (!cookie) {
        return Opened(std::move(result));
      }
      game->second.TakeCookie(*cookie);
    }
    return {};
  }

  Hub::Result Tick(qint64 now_ms) { return Opened(hub.Tick(now_ms)); }
  Hub *operator->() { return &hub; }

  Hub hub;
  std::map<quint64, LinkAuth::Client> games;
  int nonces = 0;
  int unopened = 0;

private:
  Hub::Result Opened(Hub::Result result) {
    QList<Hub::Send> opened;
    for (const auto &send : result.sends) {
      const auto found = games.find(send.to.Key());
      if (KindOf(send.datagram) == Kind::Cookie) {
        opened.append(send);
        continue;
      }
      const auto from = found == games.end()
                            ? LinkAuth::Client::From::Nobody
                            : found->second.Open(Span(send.datagram));
      if (from == LinkAuth::Client::From::Nobody) {
        ++unopened;
        continue;
      }
      const QByteArray plain =
          send.datagram.left(send.datagram.size() - LinkAuth::TrailerBytes);
      if (const auto welcome = DecodeWelcome(plain);
          welcome && welcome->accepted &&
          from == LinkAuth::Client::From::Pending) {
        found->second.Welcomed();
      }
      opened.append({send.to, plain, true});
    }
    result.sends = opened;
    return result;
  }
};

int Contracts() {
  const auto datagrams = ReadContracts(BLOODBORNE_SERVER_LINK_CONTRACTS);
  CHECK(datagrams.size() == 12);
  const auto datagram = [&](const char *name) { return datagrams.at(name); };

  const QByteArray cookie(16, '\x11');
  const QByteArray nonce(16, '\x22');
  CHECK(Encode(Hello{LinkVersion, "Kaniel", Build, "s3cr3t", cookie, nonce}) ==
        datagram("hello"));
  const auto hello = DecodeHello(datagram("hello"));
  CHECK(hello && hello->version == 3 && hello->online_id == "Kaniel" &&
        hello->build == Build && hello->secret == "s3cr3t" &&
        hello->cookie == cookie && hello->nonce == nonce);
  // Link version 3: the cookie, and a datagram's counter and tag (the keys from
  // the hello's secret, nonce, cookie and id; the game's first datagram).
  LinkAuth::Cookie cookie_bytes;
  cookie_bytes.fill(0x11);
  CHECK(EncodeCookie(cookie_bytes) == datagram("cookie"));
  CHECK(DecodeCookie(datagram("cookie")) == cookie_bytes);
  LinkAuth::Nonce nonce_bytes;
  nonce_bytes.fill(0x22);
  const auto keys =
      LinkAuth::KeysFor("s3cr3t", nonce_bytes, cookie_bytes, "Kaniel");
  LinkAuth::Session game{keys.up, keys.down};
  const QByteArray ping_bytes = Encode(Kind::Ping, Ping{7, 1000});
  CHECK(ping_bytes + Bytes(game.Seal(Span(ping_bytes))) ==
        datagram("ping.tagged"));
  LinkAuth::Session server = LinkAuth::ServerSession(keys);
  CHECK(server.Open(Span(datagram("ping.tagged"))) ==
        LinkAuth::Session::Verdict::Ok);
  // Phase 11: the old calls (friends, scores) in the link's second stream.
  CHECK(EncodeRpc(QByteArray::fromHex("0102")) == datagram("rpc"));
  CHECK(DecodeRpc(datagram("rpc")) == QByteArray::fromHex("0102"));
  CHECK(!DecodeRpc(datagram("ping")) && !DecodeCoop(datagram("rpc")));

  CHECK(Encode(Welcome{true, {}}) == datagram("welcome.ok"));
  CHECK(Encode(Welcome{false, "another build: 13ddd14d"}) ==
        datagram("welcome.refused"));
  const auto refused = DecodeWelcome(datagram("welcome.refused"));
  CHECK(refused && !refused->accepted &&
        refused->text == "another build: 13ddd14d");

  CHECK(Encode(Data{Ada127, GameFrame}) == datagram("data"));
  const auto data = DecodeData(datagram("data"));
  CHECK(data && data->peer == Ada127 && data->frame == GameFrame);

  CHECK(Encode(Kind::Ping, Ping{7, 1000}) == datagram("ping"));
  CHECK(Encode(Kind::Pong, Ping{7, 1000}) == datagram("pong"));
  const auto ping = DecodePing(Kind::Ping, datagram("ping"));
  CHECK(ping && ping->seq == 7 && ping->sent_ms == 1000);
  CHECK(!DecodePing(Kind::Ping, datagram("pong")));

  CHECK(Encode(Member{MemberState::Quiet, 3500, Ada127, "Ada"}) ==
        datagram("member.quiet"));
  CHECK(Encode(Member{MemberState::Back, 4200, Ada127, "Ada"}) ==
        datagram("member.back"));
  CHECK(Encode(Member{MemberState::Gone, 60000, Ada127, "Ada"}) ==
        datagram("member.gone"));
  const auto gone = DecodeMember(datagram("member.gone"));
  CHECK(gone && gone->state == MemberState::Gone && gone->gap_ms == 60000 &&
        gone->endpoint == Ada127 && gone->online_id == "Ada");

  for (const auto &[name, bytes] : datagrams) {
    CHECK(KindOf(bytes).has_value());
    for (int size = 0; size < bytes.size(); ++size) {
      const QByteArray cut = bytes.left(size);
      if (name == "data" && size >= 15) {
        continue; // a shorter frame is still a frame
      }
      if (name == "ping.tagged" && size >= 21) {
        continue; // a ping with bytes after it
      }
      CHECK(!DecodeHello(cut) && !DecodeWelcome(cut) && !DecodeData(cut) &&
            !DecodeCookie(cut) && !DecodePing(Kind::Ping, cut) &&
            !DecodePing(Kind::Pong, cut) && !DecodeMember(cut));
    }
  }
  // The STUN ping shares the header.
  CHECK(!KindOf(QByteArray::fromHex("ffffffff014b616e69656c")));
  std::cout << "server link contracts: ok\n";
  return 0;
}

int HelloAndBuilds() {
  Net hub;
  auto result = hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  CHECK(SentTo(result, Kaniel).size() == 1);
  CHECK(DecodeWelcome(SentTo(result, Kaniel)[0])->accepted);
  CHECK(hub->Members() == 1 && hub->Build() == Build);

  result = hub.Receive(Ada, HelloFrom("Ada"), 10);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  CHECK(hub->Members() == 2);

  // One build for everyone: the 0x60 record carries a raw vtable pointer.
  result = hub.Receive(Eileen, HelloFrom("Eileen", "13ddd14d"), 20);
  const auto refused = DecodeWelcome(SentTo(result, Eileen).value(0));
  CHECK(refused && !refused->accepted);
  CHECK(refused->text.contains("13ddd14d") && refused->text.contains(Build));
  CHECK(refused->text.contains(
      "everyone needs the same release")); // said on the team screen
  CHECK(hub->Members() == 2);

  // Another link version cannot read a tagged answer, and an untagged one
  // would go to any source: it hears nothing.
  Hello later{4, "Eileen", Build, "s3cr3t", {}, {}};
  result = hub.Receive(Eileen, Encode(later), 30);
  CHECK(result.sends.isEmpty() && hub->Members() == 2);

  // A hello again (the game restarted on the same port) is welcomed again.
  result = hub.Receive(Kaniel, HelloFrom("Kaniel"), 40);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(hub->Members() == 2);
  // Phase 11: the link starts with the sign-in, before the co-op code says the
  // build. A hello with none is welcomed and holds nothing against the others;
  // the game says hello again once it knows its build, and that one counts.
  Net fresh;
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel", ""), 45);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(fresh->Build().isEmpty());
  result = fresh.Receive(Ada, HelloFrom("Ada"), 46);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  CHECK(fresh->Build() == Build);
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel", "13ddd14d"), 47);
  CHECK(!DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel"), 48);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(fresh->Members() == 2);
  // A game of before phase 11 (link version 1, no secret) neither.
  QByteArray old = QByteArray::fromHex("ffffffff") + "SHLK";
  old += char(1);
  old += char(1);
  old += char(3);
  old += "Old";
  old += char(Build.size());
  old += Build;
  result = hub.Receive(Eileen, old, 50);
  CHECK(result.sends.isEmpty() && hub.unopened == 0);
  std::cout << "server link hello: ok\n";
  return 0;
}

// Phase 11: the hello is the sign-in. The player's id and its secret (the
// launcher made them) sign in, nothing else; the old calls go in the link's
// second stream (Rpc), from signed-in games only.
int SignsIn() {
  Net hub;
  QList<QByteArray> asked;
  hub->SetSignIn([&](const QByteArray &online_id, const QByteArray &secret) {
    asked.append(online_id);
    if (online_id == "Squatter") {
      return SignInVerdict{false, "address taken: make a new id"};
    }
    return SignInVerdict{online_id == "Kaniel" && secret == "right", {}};
  });
  auto result = hub.Receive(Kaniel, HelloFrom("Kaniel", Build, "wrong"), 0);
  const auto refused = DecodeWelcome(SentTo(result, Kaniel).value(0));
  CHECK(refused && !refused->accepted && refused->text.contains("sign-in"));
  CHECK(hub->Members() == 0 && asked == QList<QByteArray>{"Kaniel"});
  result = hub.Receive(Kaniel, EncodeRpc("dropped"), 5);
  CHECK(result.rpc.isEmpty() && result.sends.isEmpty());

  result = hub.Receive(Kaniel, HelloFrom("Kaniel", Build, "right"), 10);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(hub->Members() == 1);
  result = hub.Receive(Kaniel, EncodeRpc("body"), 20);
  CHECK(result.rpc.size() == 1 && result.rpc[0].from == Kaniel &&
        result.rpc[0].online_id == "Kaniel" && result.rpc[0].body == "body");
  CHECK(result.coop.isEmpty());
  // A refusal with its reason (a new account on another's address): the
  // game's player reads it.
  result = hub.Receive(Ada, HelloFrom("Squatter", Build, "any"), 30);
  const auto squatted = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(squatted && !squatted->accepted &&
        squatted->text == "address taken: make a new id");
  // The address the games compute too (emulator net_link.h): never changes.
  CHECK(VirtualEndpointOf("p000000000048cf").ipv4 == 0x0ae08f32 &&
        VirtualEndpointOf("p00000000005b99").ipv4 == 0x0ae08f32 &&
        VirtualEndpointOf("Kaniel").port == VirtualPort);
  std::cout << "server link sign-in: ok\n";
  return 0;
}

int Relays() {
  Net hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);

  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 100);
  const auto forwarded = SentTo(result, Ada);
  CHECK(forwarded.size() == 1 && result.sends.size() == 1);
  const auto data = DecodeData(forwarded[0]);
  CHECK(data && data->peer == VirtualEndpointOf("Kaniel") &&
        data->frame == GameFrame);
  CHECK(!result.log.isEmpty() && result.log[0].contains("Kaniel") &&
        result.log[0].contains("Ada"));

  // Nobody at the destination: dropped, and said.
  result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Eileen"), GameFrame}), 110);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("no game"));

  // A game that never said hello is not relayed.
  result = hub.Receive(
      Eileen, Encode(Data{VirtualEndpointOf("Kaniel"), GameFrame}), 120);
  CHECK(result.sends.isEmpty());

  // Only the first frames of a pair are logged.
  Net quiet_log(3000, 60000, 10000, 2);
  quiet_log.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  quiet_log.Receive(Ada, HelloFrom("Ada"), 0);
  int lines = 0;
  for (int index = 0; index < 5; ++index) {
    lines += static_cast<int>(
        quiet_log
            .Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}),
                     index)
            .log.size());
  }
  CHECK(lines == 2);

  // A ping comes back as a pong with the same body.
  result = hub.Receive(Ada, Encode(Kind::Ping, Ping{3, 777}), 130);
  const auto pong = DecodePing(Kind::Pong, SentTo(result, Ada).value(0));
  CHECK(pong && pong->seq == 3 && pong->sent_ms == 777);
  CHECK(hub.unopened == 0);
  std::cout << "server link relay: ok\n";
  return 0;
}

// Phase 11: a game silent for 12 s (its game died, or its connection is down
// for longer than a blip) loses its sign-in session (`lost`, once a silence):
// what the TCP connection's end did at once when its game died. A blip under
// that rides out.
int ALongSilenceEndsTheSignIn() {
  Net hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  for (qint64 at = 1000; at <= 30000; at += 1000) {
    hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), at);
    const auto result = hub.Tick(at + 1);
    if (at == 12000) {
      CHECK(result.lost.size() == 1 && result.lost[0] == Kaniel);
    } else {
      CHECK(result.lost.isEmpty());
    }
    if (at == 20000) {
      hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), at); // back
    }
  }
  CHECK(hub.Tick(32002).lost.size() == 1); // silent again since 20 s
  std::cout << "server link: a long silence ends the sign-in\n";
  return 0;
}

int Liveness() {
  Net hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1000}), 1000);

  auto result = hub.Tick(3000);
  CHECK(result.sends.isEmpty());

  // Ada's link quiet for longer than 3 s: Kaniel is told, once.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{2, 3000}), 3000);
  result = hub.Tick(3500);
  const auto told = SentTo(result, Kaniel);
  CHECK(told.size() == 1 && SentTo(result, Ada).isEmpty());
  const auto quiet = DecodeMember(told[0]);
  CHECK(quiet && quiet->state == MemberState::Quiet &&
        quiet->endpoint == VirtualEndpointOf("Ada") &&
        quiet->online_id == "Ada" && quiet->gap_ms == 3500);
  CHECK(!result.log.isEmpty() && result.log[0].contains("Ada"));
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{3, 4000}), 4000);
  CHECK(SentTo(hub.Tick(4500), Kaniel).isEmpty());

  // Heard again.
  result = hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 6000}), 6000);
  const auto back = DecodeMember(SentTo(result, Kaniel).value(0));
  CHECK(back && back->state == MemberState::Back && back->gap_ms == 6000);

  // Kaniel's game is gone after a minute without a word; the build is free
  // again once nobody is linked.
  hub.Receive(Ada, Encode(Kind::Ping, Ping{2, 64000}), 64000);
  result = hub.Tick(64001);
  CHECK(SentTo(result, Ada).size() == 1); // gone, not quiet first
  const auto gone = DecodeMember(SentTo(result, Ada).value(0));
  CHECK(gone && gone->state == MemberState::Gone &&
        gone->online_id == "Kaniel");
  CHECK(hub->Members() == 1);
  hub.Tick(200000);
  CHECK(hub->Members() == 0 && hub->Build().isEmpty());
  result = hub.Receive(Eileen, HelloFrom("Eileen", "13ddd14d"), 200001);
  CHECK(DecodeWelcome(SentTo(result, Eileen).value(0))->accepted);

  // The counters every 10 s.
  Net counted;
  counted.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  counted.Receive(Ada, HelloFrom("Ada"), 0);
  counted.Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 1);
  counted.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), 9000);
  counted.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), 9000);
  CHECK(counted.Tick(9999).log.isEmpty());
  const auto counters = counted.Tick(10000).log;
  CHECK(counters.size() == 2);
  CHECK(counters.join('\n').contains("Kaniel") &&
        counters.join('\n').contains("Ada"));
  CHECK(hub.unopened == 0);
  std::cout << "server link liveness: ok\n";
  return 0;
}

// Phase 7: the relay and the member notices stay inside the sender's team.
int TeamOnly() {
  Net hub;
  const Endpoint Lone{0x7F000001, 40004};
  hub->SetPartyOf([&](const Endpoint &link) -> QString {
    if (link == Kaniel || link == Ada) {
      return QStringLiteral("team-a");
    }
    return link == Eileen ? QStringLiteral("team-b") : QString{};
  });
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  hub.Receive(Eileen, HelloFrom("Eileen"), 0);
  hub.Receive(Lone, HelloFrom("Lone"), 0);

  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 10);
  CHECK(SentTo(result, Ada).size() == 1);
  // Another team's game, or one in no team yet: dropped, and said.
  result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Eileen"), GameFrame}), 20);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("another team"));
  result =
      hub.Receive(Lone, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 30);
  CHECK(result.sends.isEmpty());
  result =
      hub.Receive(Ada, Encode(Data{VirtualEndpointOf("Lone"), GameFrame}), 40);
  CHECK(result.sends.isEmpty());

  // Eileen going quiet is told to nobody in team a.
  for (const Endpoint &game : {Kaniel, Ada, Lone}) {
    hub.Receive(game, Encode(Kind::Ping, Ping{1, 3000}), 3000);
  }
  result = hub.Tick(3500);
  CHECK(result.sends.isEmpty());
  // Ada going quiet is told to Kaniel only.
  for (const Endpoint &game : {Kaniel, Eileen, Lone}) {
    hub.Receive(game, Encode(Kind::Ping, Ping{2, 6000}), 6000);
  }
  result = hub.Tick(7000);
  CHECK(SentTo(result, Kaniel).size() == 1 && result.sends.size() == 1);
  CHECK(hub.unopened == 0);
  std::cout << "server link team only: ok\n";
  return 0;
}

// Phase 7: no game learns another's address. Each game is known by a virtual
// address made from its account (10.x.y.z, one port), the same from any port
// it links from; the hub forwards between the virtual addresses.
int VirtualMembers() {
  Net hub;
  const Endpoint ada_virtual = VirtualEndpointOf("Ada");
  const Endpoint kaniel_virtual = VirtualEndpointOf("Kaniel");
  CHECK((ada_virtual.ipv4 >> 24) == 10 && ada_virtual.port == VirtualPort);
  CHECK(!(ada_virtual == kaniel_virtual));
  CHECK(VirtualEndpointOf("Ada") == ada_virtual); // the same every time
  // The games compute the same (emulator/tests/network/test_net_link.cpp).
  CHECK(VirtualEndpointOf("Kaniel").ipv4 == 0x0A89C805u);
  CHECK(ada_virtual.ipv4 == 0x0AC3A55Bu);
  CHECK(!hub->VirtualOf(Ada)); // not linked yet
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  // A game welcomed is said with its virtual address: the rooms and signaling
  // hand that one out (no STUN needed).
  const auto linked = hub.Receive(Ada, HelloFrom("Ada"), 0).linked;
  CHECK(linked.size() == 1 && linked[0].online_id == "Ada" &&
        linked[0].virtual_endpoint == ada_virtual);
  CHECK(hub->VirtualOf(Ada) == ada_virtual);

  // Sent to Ada's virtual address: Ada gets it from Kaniel's virtual address.
  auto result = hub.Receive(Kaniel, Encode(Data{ada_virtual, GameFrame}), 10);
  const auto forwarded = DecodeData(SentTo(result, Ada).value(0));
  CHECK(forwarded && forwarded->peer == kaniel_virtual &&
        forwarded->frame == GameFrame);
  // Phase 9: a real address finds nobody, even a linked game's.
  result = hub.Receive(Kaniel, Encode(Data{Ada, GameFrame}), 15);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("no game"));

  // Ada's game links again from another port: still the same address.
  const Endpoint ada_moved{0x7F000001, 40099};
  // Its old link is forgotten (the world's member there goes), told to nobody:
  // Ada is still here.
  result = hub.Receive(ada_moved, HelloFrom("Ada"), 20);
  CHECK(result.gone.size() == 1 && result.gone[0] == Ada);
  CHECK(SentTo(result, Kaniel).isEmpty() && hub->Members() == 2);
  CHECK(hub->VirtualOf(ada_moved) == ada_virtual && !hub->VirtualOf(Ada));
  result = hub.Receive(Kaniel, Encode(Data{ada_virtual, GameFrame}), 30);
  CHECK(SentTo(result, ada_moved).size() == 1 && result.sends.size() == 1);

  // The notices name the virtual address too.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 3000}), 3000);
  result = hub.Tick(3500);
  const auto quiet = DecodeMember(SentTo(result, Kaniel).value(0));
  CHECK(quiet && quiet->state == MemberState::Quiet &&
        quiet->endpoint == ada_virtual);
  CHECK(hub.unopened == 0);
  std::cout << "server link virtual members: ok\n";
  return 0;
}

// Phase 9: the hub reads each game packet in a frame (SHAD GamePacketsType)
// and forwards only what the sender's routes let reach that member: a refused
// unreliable packet leaves the frame, a refused reliable one keeps its
// sequence number (coop/packet_routes.h). Other frames go as they are.
QByteArray GamePacketsFrame(const BloodborneCoop::GameFrame &frame) {
  QByteArray bytes = QByteArray::fromHex("ffffffff") + "SHAD" +
                     char(BloodborneCoop::GamePacketsType);
  const auto payload = BloodborneCoop::EncodeGameFrame(frame);
  bytes.append(reinterpret_cast<const char *>(payload.data()),
               static_cast<qsizetype>(payload.size()));
  return bytes;
}

std::vector<u8> PacketOn(u32 channel) {
  std::vector<u8> packet;
  BloodborneCoop::GameLinkDetail::Put(packet, channel);
  BloodborneCoop::GameLinkDetail::Put(packet, u32{2});
  packet.push_back(0xAB);
  packet.push_back(0xCD);
  return packet;
}

int RoutesGamePackets() {
  using BloodborneCoop::PacketClass;
  Net hub;
  std::vector<std::tuple<Endpoint, Endpoint, u32>> asked;
  hub->SetPacketRoutes(
      [&](const Endpoint &from, const Endpoint &to, u32 channel) {
        asked.emplace_back(from, to, channel);
        return channel != 0x06 && channel != 0x31;
      });
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);

  BloodborneCoop::GameFrame frame{"Kaniel", 5, 6, 2, {}};
  frame.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x06)});
  frame.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x04)});
  frame.entries.push_back({PacketClass::Reliable, 4, PacketOn(0x31)});
  frame.entries.push_back({PacketClass::Reliable, 5, PacketOn(0x10)});
  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GamePacketsFrame(frame)}),
      10);
  CHECK(asked.size() == 4 && std::get<0>(asked[0]) == Kaniel &&
        std::get<1>(asked[0]) == Ada && std::get<2>(asked[0]) == 0x06);
  const auto data = DecodeData(SentTo(result, Ada).value(0));
  CHECK(data &&
        data->frame.startsWith(QByteArray::fromHex("ffffffff") + "SHAD" +
                               char(BloodborneCoop::GamePacketsType)));
  const auto routed = BloodborneCoop::DecodeGameFrame(
      {reinterpret_cast<const u8 *>(data->frame.constData()) + 9,
       static_cast<size_t>(data->frame.size() - 9)});
  CHECK(routed && routed->sender == "Kaniel" && routed->ack == 2 &&
        routed->entries.size() == 3);
  CHECK(routed->entries[0].packet == PacketOn(0x04));
  CHECK(routed->entries[1].seq == 4 &&
        routed->entries[1].packet == BloodborneCoop::DroppedNativePacket());
  CHECK(routed->entries[2].packet == PacketOn(0x10));

  // Nothing left but its ack: the frame still goes (the stream needs the ack).
  BloodborneCoop::GameFrame only_refused{"Kaniel", 5, 6, 3, {}};
  only_refused.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x06)});
  result = hub.Receive(
      Kaniel,
      Encode(Data{VirtualEndpointOf("Ada"), GamePacketsFrame(only_refused)}),
      20);
  CHECK(SentTo(result, Ada).size() == 1);

  // A frame that is not the game's packets is not read.
  asked.clear();
  result = hub.Receive(Kaniel,
                       Encode(Data{VirtualEndpointOf("Ada"), ::GameFrame}), 30);
  CHECK(asked.empty() &&
        DecodeData(SentTo(result, Ada).value(0))->frame == ::GameFrame);
  // A game frame that does not read is dropped, and said.
  result = hub.Receive(
      Kaniel,
      Encode(Data{VirtualEndpointOf("Ada"),
                  QByteArray::fromHex("ffffffff") + "SHAD" +
                      char(BloodborneCoop::GamePacketsType) + "\x01"}),
      40);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() &&
        result.log.join('\n').contains("does not read"));

  // The counters say how much each sender had refused, per channel.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), 9000);
  hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), 9000);
  const QString counters = hub.Tick(10000).log.join('\n');
  CHECK(counters.contains("refused") && counters.contains("0x6: 2") &&
        counters.contains("0x31: 1"));
  CHECK(hub.unopened == 0);
  std::cout << "server link routes game packets: ok\n";
  return 0;
}
// Link version 3 (coop/link_auth.h): SHA-256 and HMAC as the standards say, and
// as Qt computes them.
int Crypto() {
  const auto hex = [](const LinkAuth::Digest &digest) {
    return QByteArray(reinterpret_cast<const char *>(digest.data()), 32)
        .toHex();
  };
  CHECK(hex(LinkAuth::Sha256::Of(LinkAuth::Bytes("abc"))) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // RFC 4231, test case 2.
  CHECK(hex(LinkAuth::HmacKey{LinkAuth::Bytes("Jefe")}.Mac(
            LinkAuth::Bytes("what do ya want for nothing?"))) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  QByteArray key, message;
  for (int size = 0; size < 300; ++size) {
    key.append(char(size * 7));
    message.append(char(size * 13 + 1));
    const auto ours = LinkAuth::HmacKey{Span(key.left(size % 90))}.Mac(
        Span(message.left(size / 2)), Span(message.mid(size / 2)));
    CHECK(hex(ours) ==
          QMessageAuthenticationCode::hash(message, key.left(size % 90),
                                           QCryptographicHash::Sha256)
              .toHex());
  }
  std::cout << "server link crypto: ok\n";
  return 0;
}

// Link version 3: a hello needs a cookie of its source address and port (no
// state kept, the answer no larger than the hello); after it every datagram
// carries its link's counter and tag, and one that does not is dropped without
// a word, counted.
int Handshake() {
  Hub hub;
  int checks = 0;
  hub.SetSignIn([&](const QByteArray &, const QByteArray &) {
    ++checks;
    return SignInVerdict{true, {}};
  });
  int admitted = 0;
  hub.SetAdmit([&](const Endpoint &, qsizetype) {
    ++admitted;
    return true;
  });
  LinkAuth::Client game;
  LinkAuth::Nonce nonce{};
  nonce[3] = 9;
  game.Restart(nonce);
  const auto hello = [&](const Endpoint &, qint64) {
    Hello said{LinkVersion, "Kaniel", Build, {}, {}, {}};
    const auto &echo = game.CookieToEcho();
    if (game.HasCookie()) {
      game.ForHello("s3cr3t", "Kaniel");
      said.secret = "s3cr3t";
      said.cookie = QByteArray(reinterpret_cast<const char *>(echo.data()), 16);
    }
    said.nonce = QByteArray(reinterpret_cast<const char *>(nonce.data()), 16);
    return Encode(said);
  };

  // The first hello: a cookie back, nothing else (a spoofed source never sees
  // it: no link, no sign-in, nothing kept).
  const QByteArray first = hello(Kaniel, 0);
  auto result = hub.Receive(Kaniel, first, 1000);
  CHECK(result.sends.size() == 1 && result.log.isEmpty());
  const auto cookie = DecodeCookie(result.sends[0].datagram);
  CHECK(cookie && result.sends[0].datagram.size() < first.size());
  CHECK(hub.Members() == 0 && checks == 0 && hub.CookiesSent() == 1);
  game.TakeCookie(*cookie);

  // The cookie holds for its source only, and for 30-60 s.
  const QByteArray second = hello(Kaniel, 1000);
  CHECK(hub.CookieValid(Kaniel, second, 1000));
  CHECK(hub.CookieValid(Kaniel, second, 59'999));
  CHECK(!hub.CookieValid(Kaniel, second, 60'000));
  CHECK(!hub.CookieValid(Ada, second, 1000));
  CHECK(!hub.CookieValid(Endpoint{Kaniel.ipv4 + 1, Kaniel.port}, second, 1000));
  CHECK(!hub.CookieValid(Kaniel, first, 1000));
  result = hub.Receive(Ada, second, 1000); // someone else's cookie
  CHECK(DecodeCookie(SentTo(result, Ada).value(0)) && hub.Members() == 0 &&
        checks == 0);
  Hub restarted; // another key: the cookies of before hold nothing
  CHECK(!restarted.CookieValid(Kaniel, second, 1000));

  // The cookie echoed: signed in, and the welcome tagged for this game.
  result = hub.Receive(Kaniel, second, 2000);
  CHECK(checks == 1 && hub.Members() == 1);
  const QByteArray welcome = SentTo(result, Kaniel).value(0);
  CHECK(game.Open(Span(welcome)) == LinkAuth::Client::From::Pending);
  CHECK(DecodeWelcome(welcome.left(welcome.size() - 24))->accepted);
  game.Welcomed();

  const auto sealed = [&](const QByteArray &plain) {
    return plain + Bytes(*game.Seal(Span(plain)));
  };
  const QByteArray ping = sealed(Encode(Kind::Ping, Ping{1, 1}));
  result = hub.Receive(Kaniel, ping, 2100);
  CHECK(SentTo(result, Kaniel).size() == 1 && admitted == 1);
  CHECK(game.Open(Span(SentTo(result, Kaniel)[0])) ==
        LinkAuth::Client::From::Current);

  // The same datagram again (a replay): dropped, counted.
  result = hub.Receive(Kaniel, ping, 2200);
  CHECK(result.sends.isEmpty() && result.log.isEmpty() && hub.Replays() == 1 &&
        admitted == 1);
  // A bad tag, or none (someone sending from the game's address): dropped
  // before its rates count it.
  QByteArray forged = sealed(EncodeCoop("restart"));
  forged[forged.size() - 1] = char(forged[forged.size() - 1] ^ 1);
  result = hub.Receive(Kaniel, forged, 2300);
  CHECK(result.coop.isEmpty() && result.sends.isEmpty() && hub.BadTags() == 1);
  result = hub.Receive(Kaniel, EncodeRpc("injected"), 2400);
  CHECK(result.rpc.isEmpty() && hub.BadTags() == 2 && admitted == 1);
  QByteArray moved_bytes = sealed(EncodeCoop("body"));
  result = hub.Receive(Ada, moved_bytes, 2450); // from another source
  CHECK(result.coop.isEmpty());
  // A tag good, the stream goes on.
  result = hub.Receive(Kaniel, sealed(EncodeCoop("body")), 2500);
  CHECK(result.coop.size() == 1 && result.coop[0].body == "body");

  // Late datagrams within the window are taken, once; older ones are not.
  QList<QByteArray> burst;
  for (int index = 0; index < 1100; ++index) {
    burst.append(sealed(EncodeCoop(QByteArray::number(index))));
  }
  CHECK(hub.Receive(Kaniel, burst[1099], 3000).coop.size() == 1);
  CHECK(hub.Receive(Kaniel, burst[500], 3000).coop.size() == 1);
  CHECK(hub.Receive(Kaniel, burst[500], 3000).coop.isEmpty());
  CHECK(hub.Receive(Kaniel, burst[10], 3000).coop.isEmpty());
  CHECK(hub.Replays() == 3);
  CHECK(hub.Receive(Kaniel, burst[1098], 3000).coop.size() == 1);

  // The hello repeated (the same nonce and cookie): one session, its counters
  // kept, so a datagram of before is still a replay.
  result = hub.Receive(Kaniel, hello(Kaniel, 3100), 3100);
  CHECK(game.Open(Span(SentTo(result, Kaniel).value(0))) ==
        LinkAuth::Client::From::Current);
  CHECK(hub.Receive(Kaniel, burst[1099], 3200).coop.isEmpty());

  // A refusal is tagged for the game that asked, a hello without a cookie
  // gets none.
  CHECK(!hub.Refusal(Kaniel, first, "too many", 3300));
  const auto refusal = hub.Refusal(Kaniel, second, "too many", 3300);
  LinkAuth::Client fresh;
  fresh.Restart(nonce);
  fresh.TakeCookie(*cookie);
  fresh.ForHello("s3cr3t", "Kaniel");
  CHECK(refusal &&
        fresh.Open(Span(*refusal)) == LinkAuth::Client::From::Pending);
  // What the hub sends a game outside its answers (the worlds, Rpc): tagged
  // for a linked game, nothing for another.
  CHECK(!hub.Seal(Ada, EncodeCoop("x")));
  CHECK(game.Open(Span(*hub.Seal(Kaniel, EncodeCoop("x")))) ==
        LinkAuth::Client::From::Current);
  std::cout << "server link handshake: ok\n";
  return 0;
}
} // namespace

int main() {
  if (Contracts() != 0 || HelloAndBuilds() != 0 || Relays() != 0 ||
      Liveness() != 0 || TeamOnly() != 0 || VirtualMembers() != 0 ||
      RoutesGamePackets() != 0 || SignsIn() != 0 ||
      ALongSilenceEndsTheSignIn() != 0 || Crypto() != 0 || Handshake() != 0) {
    return 1;
  }
  return 0;
}
