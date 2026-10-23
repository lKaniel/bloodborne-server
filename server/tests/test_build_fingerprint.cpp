// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The build fingerprint (coop/build_fingerprint.h) the games and shadNet
// compare at the link's hello: its text, the differences and the refusal the
// player reads.
#include <iostream>
#include <string>

#include "coop/build_fingerprint.h"

using namespace BloodborneCoop;

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

BuildFingerprint Game() {
  BuildFingerprint build;
  build.Set("version", "1.2.0");
  build.Set("revision", "08e56cd82c5c4f7d1d2e3f4a5b6c7d8e9f0a1b2c");
  build.Set("protocol", "9");
  build.Set("eboot", "1f2e3d4c5b6a7980");
  build.Set("events", "0123456789abcdef");
  build.Set("harder", "off");
  build.Set("cheats", "none");
  build.Set("rules", "lamp rests off, revive hits real");
  build.Set("fps", "60");
  build.Set("patches", "60 FPS++, Larger GX Per-Frame Command Heap");
  return build;
}

BuildFingerprint Server() {
  BuildFingerprint build;
  build.Set("version", "1.2.0");
  build.Set("revision", "08e56cd82c5c4f7d1d2e3f4a5b6c7d8e9f0a1b2c");
  build.Set("protocol", "9");
  return build;
}

int Text() {
  const BuildFingerprint game = Game();
  const std::string text = EncodeBuildFingerprint(game);
  CHECK(text.starts_with("bb-build 1;version=1.2.0;revision=08e56cd8"));
  CHECK(text.size() <= BuildTextMax);
  const auto parsed = ParseBuildFingerprint(text);
  CHECK(parsed && *parsed == game);
  CHECK(parsed->Get("harder") == "off" && parsed->Get("absent").empty());
  // A value cannot break the text: ';' and '=' become ',' and ':'.
  BuildFingerprint odd;
  odd.Set("cheats", "a;b=c");
  CHECK(odd.Get("cheats") == "a,b:c");
  odd.Set("cheats", "again");
  CHECK(odd.fields.size() == 1 && odd.Get("cheats") == "again");
  // The hello's text before the fingerprint ("<revision> events <hash>
  // protocol <n>") and other text are not a fingerprint.
  CHECK(!ParseBuildFingerprint("92829ac4 events 1f protocol 9"));
  CHECK(!ParseBuildFingerprint(""));
  CHECK(!ParseBuildFingerprint("bb-build 2;version=9"));
  CHECK(!ParseBuildFingerprint("bb-build 1;version"));
  // The link's text holds 255 bytes: what the comparison needs always goes,
  // the fields only logged give way.
  BuildFingerprint full = Game();
  // (The games say a revision's first 12 characters, the files' hashes in
  // 12 hex digits.)
  full.Set("revision", std::string(12, 'a') + "-dirty");
  full.Set("eboot", std::string(12, 'e'));
  full.Set("events", std::string(12, 'f'));
  full.Set("harder", "10,10,10");
  full.Set("cheats",
           "no death, stealth, silent, rally no decay, enemy control, "
           "no rally");
  full.Set("rules", "lamp rests on, revive hits presses, guest as host");
  full.Set("patches", std::string(300, 'p'));
  const std::string clipped = EncodeBuildFingerprint(full);
  CHECK(clipped.size() <= BuildTextMax);
  const auto kept = ParseBuildFingerprint(clipped);
  CHECK(kept && kept->Get("rules") == full.Get("rules") &&
        kept->Get("cheats") == full.Get("cheats") &&
        kept->Get("patches").empty());
  std::cout << "build fingerprint text: ok\n";
  return 0;
}

int Differences() {
  const BuildFingerprint game = Game();
  BuildFingerprint other = Game();
  CHECK(CheckedDifferences(game, other).empty());
  CHECK(DescribeDifferences(game, other).empty());
  other.Set("harder", "3,1.5,3");
  other.Set("fps", "90 unlocked");
  const auto differences = CheckedDifferences(game, other);
  CHECK(differences.size() == 1 && differences[0].key == "harder" &&
        differences[0].mine == "off" && differences[0].theirs == "3,1.5,3");
  // Only the fields that change the game are checked; the frame rate is
  // only said.
  const std::string said = DescribeDifferences(game, other);
  CHECK(said == "harder-enemies setting off, theirs HP x3 damage x1.5 poise "
                "x3; not checked: frame rate 60, theirs 90 unlocked");
  // An unknown value (a file that could not be read) holds nothing.
  other = Game();
  other.Set("eboot", "");
  CHECK(CheckedDifferences(game, other).empty());
  // The server knows its release only.
  other = Game();
  other.Set("harder", "3,1.5,3");
  other.Set("version", "1.1.9");
  CHECK(CheckedDifferences(game, other, true).size() == 1);
  std::cout << "build fingerprint differences: ok\n";
  return 0;
}

int Refusals() {
  const BuildFingerprint server = Server();
  BuildFingerprint here = Game();
  BuildFingerprint game = Game();
  // Nothing differs: welcome.
  CHECK(BuildRefusalOf(EncodeBuildFingerprint(game), &server, &here)
            .text.empty());
  CHECK(BuildRefusalOf(EncodeBuildFingerprint(game), nullptr, nullptr)
            .text.empty());

  // An older release than the server and the games here: the version alone
  // (another release explains every other difference).
  game.Set("version", "1.1.9");
  game.Set("revision", "77e35074aaaabbbbccccddddeeeeffff00001111");
  game.Set("events", "fedcba9876543210");
  auto refusal = BuildRefusalOf(EncodeBuildFingerprint(game), &server, &here);
  CHECK(refusal.text == "another build: your game is version 1.1.9, the "
                        "server and the games here run 1.2.0: update in the "
                        "launcher");
  CHECK(refusal.notice == "Co-op refused: your game is version 1.1.9, the "
                          "server runs 1.2.0: update in the launcher");
  // The server alone (nobody linked yet).
  refusal = BuildRefusalOf(EncodeBuildFingerprint(game), &server, nullptr);
  CHECK(refusal.text == "another build: your game is version 1.1.9, the "
                        "server runs 1.2.0: update in the launcher");
  // The server's own build not checked (a dev or rig server): the games here.
  refusal = BuildRefusalOf(EncodeBuildFingerprint(game), nullptr, &here);
  CHECK(refusal.text == "another build: your game is version 1.1.9, the games "
                        "here run 1.2.0: update in the launcher");

  // The same release on another build: the revision.
  game = Game();
  game.Set("revision", "77e35074aaaabbbbccccddddeeeeffff00001111");
  refusal = BuildRefusalOf(EncodeBuildFingerprint(game), &server, &here);
  CHECK(refusal.text == "another build: your game is build 77e35074, the "
                        "server and the games here run 08e56cd8: use the "
                        "same release");

  // A setting that changes the game: against the games here, with both
  // values.
  game = Game();
  game.Set("harder", "3,1.5,3");
  refusal = BuildRefusalOf(EncodeBuildFingerprint(game), &server, &here);
  CHECK(refusal.text ==
        "another build: your harder-enemies setting is HP x3 damage x1.5 poise "
        "x3, the games here use off: change it in the launcher's co-op "
        "settings");
  CHECK(refusal.notice.size() <= RefusalNoticeMax);
  CHECK(refusal.notice.starts_with("Co-op refused: your harder enemies"));
  // The server holds no setting: alone on it, the game is welcome.
  CHECK(BuildRefusalOf(EncodeBuildFingerprint(game), &server, nullptr)
            .text.empty());

  // Game files: which ones, and every field in order.
  game = Game();
  game.Set("eboot", "aaaaaaaaaaaaaaaa");
  game.Set("events", "bbbbbbbbbbbbbbbb");
  game.Set("cheats", "no death");
  refusal = BuildRefusalOf(EncodeBuildFingerprint(game), &server, &here);
  CHECK(refusal.text ==
        "another build: your game files differ from the games here (eboot, "
        "map events): repair the game files; your cheats and tweaks are no "
        "death, the games here use none: use the same ones in the launcher");
  CHECK(refusal.notice == "Co-op refused: your game files differ (eboot, map "
                          "events): repair the game files (+1 more)");

  // A hello in the text before the fingerprint: an older release, said
  // plainly.
  refusal = BuildRefusalOf("92829ac4 events 1f protocol 9", &server, &here);
  CHECK(refusal.text == "another build: your game is another release (it "
                        "says \"92829ac4 events 1f protocol 9\"): update in "
                        "the launcher");
  CHECK(refusal.notice ==
        "Co-op refused: your game is another release: update in the launcher");
  std::cout << "build refusals: ok\n";
  return 0;
}

// The team screen shows the notice on one line of RefusalNoticeMax
// characters, whatever the values.
int NoticesFit() {
  const BuildFingerprint server = Server();
  const BuildFingerprint here = Game();
  const std::string long_value(120, 'x');
  for (const auto &field : BuildFields) {
    if (!field.checked) {
      continue;
    }
    for (const bool server_too : {false, true}) {
      BuildFingerprint game = Game();
      game.Set(field.key, long_value);
      game.Set("rules", "lamp rests on, revive hits presses, guest as host");
      game.Set("cheats", "no death, stealth, silent, rally no decay");
      const auto refusal = BuildRefusalOf(
          EncodeBuildFingerprint(game), server_too ? &server : nullptr, &here);
      CHECK(!refusal.text.empty());
      CHECK(refusal.notice.size() <= RefusalNoticeMax);
      CHECK(refusal.notice.starts_with("Co-op refused: "));
    }
  }
  BuildFingerprint game = Game();
  game.Set("harder", "2,1.25,4");
  const auto refusal =
      BuildRefusalOf(EncodeBuildFingerprint(game), &server, &here);
  CHECK(refusal.notice.size() <= RefusalNoticeMax);
  std::cout << "build notices fit: ok\n";
  return 0;
}

int ReleaseVersion() {
  CHECK(ReleaseVersionOf("Yharnam Together v1.2.0 (08e56cd8)") == "1.2.0");
  CHECK(ReleaseVersionOf("v0.3.10-local\nsecond line v9.9.9") ==
        "0.3.10-local");
  CHECK(ReleaseVersionOf("no version here").empty());
  CHECK(ReleaseVersionOf("").empty());
  CHECK(ReleaseVersionOf("v1.2").empty());
  CHECK(FingerprintRevision("08e56cd82c5c4f7d1d2e3f4a5b6c7d8e9f0a1b2c") ==
        "08e56cd82c5c");
  CHECK(FingerprintRevision("08e56cd82c5c4f7d-dirty") == "08e56cd82c5c-dirty");
  CHECK(FingerprintRevision("0000000").empty() &&
        FingerprintRevision("").empty());
  const BuildFingerprint server = ServerBuildFingerprint("", "0000000");
  CHECK(server.Get("version") == "dev" && server.Get("revision").empty() &&
        server.Get("protocol") == std::to_string(CoopFrameProtocol));
  // An unknown revision holds nothing against a game.
  CHECK(CheckedDifferences(Game(), server, true).size() == 1);
  std::cout << "release version: ok\n";
  return 0;
}

} // namespace

int main() {
  if (Text() != 0 || Differences() != 0 || Refusals() != 0 ||
      NoticesFit() != 0 || ReleaseVersion() != 0) {
    return 1;
  }
  return 0;
}
