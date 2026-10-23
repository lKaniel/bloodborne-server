// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/coop_types.h"

// Build check on join (Kaniel 2026-10-24): everyone, the server included, runs one identical
// build. Each game says its build in the server link's hello as a fingerprint, key=value fields
// after "bb-build 1;" (the hello's build text, at most 255 bytes); shadNet compares a new game with
// the games linked and with its own release, and refuses one that differs, naming each field with
// both values. The games log the same differences from their peers' hellos.
namespace BloodborneCoop {

// Raised when a co-op frame's meaning changes (the history is at the game's PeerHelloType). Both
// sides compare it: the server knows its own.
constexpr u8 CoopFrameProtocol = 9;

constexpr std::string_view BuildTextStart = "bb-build 1;";
// The link's texts carry a length byte.
constexpr std::size_t BuildTextMax = 255;
// The team screen's notice line (bbcoop_lobby::notice holds one more byte).
constexpr std::size_t RefusalNoticeMax = 95;

// The fields, in the order a refusal names them. `checked` ones change the game (another build,
// other game files, other rules): a difference refuses the game. The rest are only logged
// (purely visual settings). `server` ones the server knows of its own build.
struct BuildField {
    std::string_view key;
    std::string_view label;   // in a log line: "<label> <mine>, theirs <theirs>"
    std::string_view subject; // the refusal: "<subject> <mine>, <who> <verb> <theirs>: <advice>"
    std::string_view verb;
    std::string_view advice;
    std::string_view short_subject; // the notice's
    std::string_view short_label;   // the notice's "your <short label> differs"
    bool checked;
    bool server;
};
inline constexpr std::array<BuildField, 12> BuildFields{{
    {"version", "version", "your game is version", "run", "update in the launcher",
     "your game is version", "version", true, true},
    {"protocol", "co-op protocol", "your game speaks co-op protocol", "speak",
     "update in the launcher", "your co-op protocol is", "co-op protocol", true, true},
    {"revision", "build", "your game is build", "run", "use the same release", "your game is build",
     "build", true, true},
    {"eboot", "eboot", "", "", "repair the game files", "", "game files", true, false},
    {"events", "map events", "", "", "repair the game files", "", "game files", true, false},
    {"harder", "harder-enemies setting", "your harder-enemies setting is", "use",
     "change it in the launcher's co-op settings", "your harder enemies are",
     "harder-enemies setting", true, false},
    {"cheats", "cheats and tweaks", "your cheats and tweaks are", "use",
     "use the same ones in the launcher", "your cheats are", "cheats", true, false},
    {"rules", "co-op rules", "your co-op rules are", "use",
     "everyone needs the same co-op settings", "your co-op rules are", "co-op rules", true, false},
    {"fps", "frame rate", "", "", "", "", "", false, false},
    {"mods", "mods", "", "", "", "", "", false, false},
    {"patches", "other patches", "", "", "", "", "", false, false},
    {"pbin", "patches.bin", "", "", "", "", "", false, false},
}};

inline const BuildField* FindBuildField(std::string_view key) {
    for (const BuildField& field : BuildFields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

struct BuildFingerprint {
    std::vector<std::pair<std::string, std::string>> fields; // in the order first set

    // ';' and '=' in a value become ',' and ':', control characters spaces.
    void Set(std::string_view key, std::string_view value) {
        std::string clean{value};
        for (char& c : clean) {
            c = c == ';' ? ',' : c == '=' ? ':' : static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
        }
        for (auto& [k, v] : fields) {
            if (k == key) {
                v = std::move(clean);
                return;
            }
        }
        fields.emplace_back(std::string{key}, std::move(clean));
    }
    // "" when absent: unknown, never compared.
    std::string Get(std::string_view key) const {
        for (const auto& [k, v] : fields) {
            if (k == key) {
                return v;
            }
        }
        return {};
    }
    bool operator==(const BuildFingerprint&) const = default;
};

// The hello's text. The fields only logged give way (from the last) when it would pass
// BuildTextMax; the checked ones always fit.
inline std::string EncodeBuildFingerprint(const BuildFingerprint& build) {
    auto encode = [&](bool with_info, std::size_t info_fields) {
        std::string text{BuildTextStart};
        std::size_t info = 0;
        for (const auto& [key, value] : build.fields) {
            const BuildField* field = FindBuildField(key);
            if (field == nullptr || !field->checked) {
                if (!with_info || info++ >= info_fields) {
                    continue;
                }
            }
            text += key + "=" + value + ";";
        }
        return text;
    };
    std::size_t info_fields = build.fields.size();
    std::string text = encode(true, info_fields);
    while (text.size() > BuildTextMax && info_fields > 0) {
        text = encode(true, --info_fields);
    }
    return text.size() > BuildTextMax ? text.substr(0, BuildTextMax) : text;
}

// Nothing for a text that is not a fingerprint (an older release's hello says
// "<revision> events <hash> protocol <n>").
inline std::optional<BuildFingerprint> ParseBuildFingerprint(std::string_view text) {
    if (!text.starts_with(BuildTextStart)) {
        return std::nullopt;
    }
    text.remove_prefix(BuildTextStart.size());
    BuildFingerprint build;
    while (!text.empty()) {
        const std::size_t end = text.find(';');
        const std::string_view field = text.substr(0, end);
        const std::size_t equals = field.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            return std::nullopt;
        }
        build.fields.emplace_back(std::string{field.substr(0, equals)},
                                  std::string{field.substr(equals + 1)});
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    }
    return build;
}

// A value as a player reads it: a revision's first 8 characters (and "-dirty"), the harder
// setting's multipliers ("3,1.5,3": "HP x3 damage x1.5 poise x3").
inline std::string ShownBuildValue(std::string_view key, const std::string& value) {
    if (key == "revision" && value.size() > 8) {
        return value.substr(0, 8) + (value.ends_with("-dirty") ? "-dirty" : "");
    }
    if (key == "harder") {
        std::vector<std::string> parts;
        for (std::size_t start = 0;;) {
            const std::size_t comma = value.find(',', start);
            parts.push_back(value.substr(start, comma - start));
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        if (parts.size() == 3) {
            return "HP x" + parts[0] + " damage x" + parts[1] + " poise x" + parts[2];
        }
    }
    return value;
}

struct BuildDifference {
    std::string key;
    std::string mine;
    std::string theirs;
};

// The checked fields (with `server_only`, those a server knows) where `mine` and `theirs` hold
// other values, in BuildFields' order. A field unknown on either side is not compared.
inline std::vector<BuildDifference> CheckedDifferences(const BuildFingerprint& mine,
                                                       const BuildFingerprint& theirs,
                                                       bool server_only = false) {
    std::vector<BuildDifference> differences;
    for (const BuildField& field : BuildFields) {
        if (!field.checked || (server_only && !field.server)) {
            continue;
        }
        std::string a = mine.Get(field.key);
        std::string b = theirs.Get(field.key);
        if (!a.empty() && !b.empty() && a != b) {
            differences.push_back({std::string{field.key}, std::move(a), std::move(b)});
        }
    }
    return differences;
}

// The log's line on another game's build: each checked field that differs, then each logged one
// ("not checked: ..."); "" when nothing differs.
inline std::string DescribeDifferences(const BuildFingerprint& mine,
                                       const BuildFingerprint& theirs) {
    std::string checked;
    std::string logged;
    for (const BuildField& field : BuildFields) {
        const std::string a = mine.Get(field.key);
        const std::string b = theirs.Get(field.key);
        if (a == b || (field.checked && (a.empty() || b.empty()))) {
            continue;
        }
        std::string& line = field.checked ? checked : logged;
        line += std::string{line.empty() ? "" : "; "} + std::string{field.label} + " " +
                (a.empty() ? "?" : ShownBuildValue(field.key, a)) + ", theirs " +
                (b.empty() ? "?" : ShownBuildValue(field.key, b));
    }
    if (logged.empty()) {
        return checked;
    }
    return checked + (checked.empty() ? "" : "; ") + "not checked: " + logged;
}

struct BuildRefusal {
    std::string text;   // the server's log and the player's log
    std::string notice; // one line for the team screen, at most RefusalNoticeMax characters
};

// Why shadNet refuses a game whose hello says `game`: against its own build (`server`, null when
// it does not check it) and the games linked here (`here`, null when none). Another release (the
// version, protocol or revision) is named alone: it explains every other difference. Empty when
// nothing differs.
inline BuildRefusal BuildRefusalOf(std::string_view game, const BuildFingerprint* server,
                                   const BuildFingerprint* here) {
    constexpr std::string_view Start = "another build: ";
    constexpr std::string_view NoticeStart = "Co-op refused: ";
    const auto parsed = ParseBuildFingerprint(game);
    if (!parsed) {
        return {std::string{Start} + "your game is another release (it says \"" +
                    std::string{game} + "\"): update in the launcher",
                std::string{NoticeStart} + "your game is another release: update in the launcher"};
    }
    const std::vector<BuildDifference> from_server =
        server ? CheckedDifferences(*parsed, *server, true) : std::vector<BuildDifference>{};
    const std::vector<BuildDifference> from_here =
        here ? CheckedDifferences(*parsed, *here) : std::vector<BuildDifference>{};
    const auto find = [](const std::vector<BuildDifference>& list,
                         std::string_view key) -> const BuildDifference* {
        for (const BuildDifference& difference : list) {
            if (difference.key == key) {
                return &difference;
            }
        }
        return nullptr;
    };
    const auto differs = [&](std::string_view key) {
        return find(from_server, key) != nullptr || find(from_here, key) != nullptr;
    };
    const bool release = differs("version") || differs("protocol") || differs("revision");

    // One clause per field (the game files together): its long form and the notice's forms.
    struct Clause {
        std::string text;
        std::vector<std::string> notices; // the longest first
    };
    std::vector<Clause> clauses;
    std::vector<std::string> files;
    for (const BuildField& field : BuildFields) {
        if (!field.checked || (release && !field.server) ||
            (field.key == "revision" && differs("version"))) {
            continue;
        }
        const BuildDifference* by_server = find(from_server, field.key);
        const BuildDifference* by_here = find(from_here, field.key);
        if (by_server == nullptr && by_here == nullptr) {
            continue;
        }
        if (field.subject.empty()) {
            files.emplace_back(field.label);
            continue;
        }
        const std::string mine = ShownBuildValue(field.key, parsed->Get(field.key));
        const auto shown = [&](const BuildDifference* d) {
            return ShownBuildValue(field.key, d->theirs);
        };
        const std::string verb{field.verb};
        // Who holds what: "the server and the games here run 1.2.0", "the server runs 1.2.0, the
        // games here run 1.1.9".
        std::string who_long;
        std::string who_short;
        if (by_server != nullptr && by_here != nullptr && by_server->theirs == by_here->theirs) {
            who_long = "the server and the games here " + verb + " " + shown(by_server);
            who_short = "the server " + verb + "s " + shown(by_server);
        } else if (by_server != nullptr) {
            who_long = "the server " + verb + "s " + shown(by_server) +
                       (by_here ? ", the games here " + verb + " " + shown(by_here) : "");
            who_short = "the server " + verb + "s " + shown(by_server);
        } else {
            who_long = "the games here " + verb + " " + shown(by_here);
            who_short = "the others " + verb + " " + shown(by_here);
        }
        const std::string advice{field.advice};
        const std::string start = std::string{field.short_subject} + " " + mine + ", ";
        clauses.push_back(
            {std::string{field.subject} + " " + mine + ", " + who_long + ": " + advice,
             {start + who_long + ": " + advice, start + who_short + ": " + advice,
              start + who_short, "your " + std::string{field.short_label} + " differs: " + advice,
              "your " + std::string{field.short_label} + " differs"}});
    }
    if (!files.empty()) {
        std::string names;
        for (const std::string& name : files) {
            names += (names.empty() ? "" : ", ") + name;
        }
        const std::string advice{FindBuildField("eboot")->advice};
        // First: only another release comes before the game files, and it is named alone.
        clauses.insert(clauses.begin(), Clause{"your game files differ from the games here (" +
                                                   names + "): " + advice,
                                               {"your game files differ (" + names + "): " + advice,
                                                "your game files differ (" + names + ")",
                                                "your game files differ: " + advice}});
    }
    if (clauses.empty()) {
        return {};
    }
    BuildRefusal refusal{std::string{Start}, {}};
    for (std::size_t i = 0; i < clauses.size(); ++i) {
        refusal.text += (i == 0 ? "" : "; ") + clauses[i].text;
    }
    const std::string more =
        clauses.size() > 1 ? " (+" + std::to_string(clauses.size() - 1) + " more)" : "";
    for (const std::string& notice : clauses.front().notices) {
        const std::string line = std::string{NoticeStart} + notice + more;
        if (line.size() <= RefusalNoticeMax) {
            refusal.notice = line;
            return refusal;
        }
    }
    refusal.notice = std::string{NoticeStart} + "your build differs" + more;
    return refusal;
}

// A source revision as the fingerprint holds it: its first 12 characters and "-dirty" (changes
// not in a commit); "" when the build did not know it ("0000000").
inline std::string FingerprintRevision(std::string_view revision) {
    const bool dirty = revision.ends_with("-dirty");
    if (dirty) {
        revision.remove_suffix(6);
    }
    if (revision.empty() || revision.find_first_not_of('0') == std::string_view::npos) {
        return {};
    }
    return std::string{revision.substr(0, 12)} + (dirty ? "-dirty" : "");
}

// shadNet's own build: what it knows of the fields (BuildField::server). `version` "" is "dev",
// as a game's without a release.
inline BuildFingerprint ServerBuildFingerprint(std::string_view version,
                                               std::string_view revision) {
    BuildFingerprint build;
    build.Set("version", version.empty() ? "dev" : version);
    build.Set("revision", FingerprintRevision(revision));
    build.Set("protocol", std::to_string(CoopFrameProtocol));
    return build;
}

// The release's version from VERSION.txt's text ("... v1.2.0 ..." on its first line), "" when it
// names none: as the launcher reads it (coop_release_version).
inline std::string ReleaseVersionOf(std::string_view version_txt) {
    const std::string_view line = version_txt.substr(0, version_txt.find('\n'));
    for (std::size_t at = 0; (at = line.find('v', at)) != std::string_view::npos; ++at) {
        std::size_t end = at + 1;
        int dots = 0;
        bool digit = false;
        while (end < line.size() &&
               (std::isdigit(static_cast<unsigned char>(line[end])) || line[end] == '.')) {
            if (line[end] == '.') {
                if (!digit) {
                    break;
                }
                ++dots;
                digit = false;
            } else {
                digit = true;
            }
            ++end;
        }
        if (dots < 2 || !digit) {
            continue;
        }
        // Then anything up to a space (\S*), as the launcher's pattern.
        while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) {
            ++end;
        }
        return std::string{line.substr(at + 1, end - at - 1)};
    }
    return {};
}

} // namespace BloodborneCoop
