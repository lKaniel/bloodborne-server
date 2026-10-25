# A hosted server

Today a host player runs shadNet at home and friends connect over Radmin. A hosted server is one
shadNet on the internet that strangers use: every team's world on one machine, kept apart. This
page is what the `hosted-server` branch adds for that, what the load tests showed, and what is
still missing before a public launch.

## `Hosted=true`

Off by default: a host's own server behaves as before. On (`shadnet.cfg`):

| What | Where | Why |
| --- | --- | --- |
| Passwords checked on a thread pool, not the link's thread; a wrong one is not hashed again for 30 s; at most 512 checks queued | `link_server.cpp` `SignIn` | each new id cost ~3 password hashes (100k rounds) on the thread that relays every game: 30 new players froze pings for 1.7 s |
| Hellos an address (4/s, burst 20), games an address (8), new accounts an address an hour (5) | `admission.h` | bots making accounts; one PC flooding hellos |
| Each link's datagrams (1500/s) and bytes (400 KB/s); past them dropped, 5 seconds past them in a minute bans the address for 15 min | `admission.h` | a game sends ~60-200 datagrams and ~10 KB a second |
| `banned.txt`: one address or player id a line (`until=<unix s>` ends one), read every 30 s; a linked game it bans is unlinked then, not only at its next hello (only its bans: an address's rate ban holds its next hellos, it does not unlink every game behind it) | `link_server.cpp`, `admin.h` `KickBanned` | the operator's bans |
| `HostedBuild`: the build every game must say | `server_link.cpp` | otherwise the first game to link decides it |
| Lobby and party names must be the player's own id (hex, as the games say them) | `bloodborne_world.cpp` `OwnName` | anyone could act in the lobby, or say hello into a party, as another player |
| A party hello must be for the world the player's team plays | `bloodborne_world.cpp` `OnHello` | any player could join any world by its key |
| A world is its players' (`TeamWorldInfo.members`): listed to them only, forgotten only by them (each for itself); a player in more than `HostedWorldsPerPlayer` leaves its oldest at a Start (one nobody keeps is put aside) | `team_lobby.cpp` | every world was listed to everyone; anyone could forget anyone's; 16 worlds server-wide; Create/Start/Leave again and again grew the list without end |
| The HTTP ports (20443 the game's services, 31315 summons and WebAPI) answer only addresses with a signed-in link, and only as a player linked from there (the backend's `UserId` or `PlatformAccountId`, the summon's `UserId`, the bearer token's account); 403 otherwise. GETs (ss.info, status) act as nobody and stay open | `http_gate.h` | the player id came from the request: anyone reaching the ports could act as any player |
| Crash reports (`/bb/report`) off unless `HostedReports` (then 8 MB each); the stats page off unless `HostedStats` | `webapi_server.cpp`, `server.cpp` | a report costs a password hash; the stats page lists player ids |
| The lobby goes out at most every 250 ms, only to players whose own state changed, listing their team, invites and 32 others | `team_lobby.cpp` `StateFor`, `bloodborne_world.cpp` `SendLobby` | every lobby change sent every player the whole server's list: O(n^2), 216 Mbit/s at 120 players |
| A trade only between members of one started team | `bloodborne_world.cpp` `TakeTrade` | trades go by member name; a stranger in a party could be offered goods or answer for another |
| Party worlds in memory: at most `HostedPartyWorlds` (2048) | `bloodborne_world.h` `Limits` | every team's world on one machine |
| The operator's commands on a UNIX-domain socket only the server's user opens (`HostedAdminSocket`, never a network port; `server/hosting/admin.sh`): list players, kick, ban/unban by id or address with an optional end (written to `banned.txt`, so restarts keep them), list worlds (members, last played, bytes on disk, measured off the link thread) and those put aside, restore one (its folder moved back at once), stats (players, worlds, link thread load, refusals); a second server started on a live socket is refused it instead of taking it | `admin.h`, `link_server.cpp` | only `banned.txt` and a restart before; a forgotten world could only come back by hand |
| Worlds nobody started for `HostedRetireDays` days (0, the default: never) are put aside once a day, as a forgotten world's folder (writer thread, logged, never deleted); a world put aside keeps its line beside it (`kept_world.pb`, with the players it had when it went: a forgotten one names its last player), so `restore` gives it back to them | `bloodborne_world.cpp` `Service::Retire` | every world ever played stays on disk and on its players' lists |
| A summon request (create, get, delete, request) must name its player (`UserId`) | `webapi_routes_bloodborne.cpp` `Refused` | one naming nobody passed the gate and could remove or claim another player's sign by its `SessionId` |

Two fixes are on for everyone: a TCP packet size under its header no longer loops the session for
ever (`client_session.cpp`), and a datagram from one game no longer walks every member's stream
(`Service::FlushSome`; that walk was the limit past ~600 games). The worlds' disk writes (journal
records, Compact, trades, team worlds, the world list) run on a writer thread
(`bloodborne_world.h` `WorldWriter`): one queue in order, the journals fsynced together every
100 ms or 256 records and always before a world.pb is written; SIGTERM/Ctrl+C drain it before
exit. A power cut can lose the last ~100 ms of records (never their order, never the journal).

Caps, also for everyone (one game must not grow the server's memory without end; each is far
above what a game sends, so only a broken or hostile one meets it):

| What | Cap | Past it | Where |
| --- | --- | --- | --- |
| A Coop message (the largest a game sends: its TeamWorld, every world flag on; a portrait is 48 KiB) | 1 MiB | the member's stream is dropped; its game starts over and says hello again | `bloodborne_world.h` `Limits` |
| A stream chunk | 1001 bytes (what a Send makes) | the same | `coop_stream.h` |
| Chunks held out of order | 1 MiB a member (Rpc: 256 KiB) | dropped, they come again as resends | `game_link.h` |
| Unreliable channels remembered | 16 | dropped (the games send none on these streams) | `game_link.h` |
| What the server queued to a game not acked | 8 MiB | the member is dropped | `Service::FlushSome` |
| An Rpc message (the game's bridge reads 16 KiB) | 64 KiB | the tunnel is dropped; the game connects again | `link_server.h` |
| Rpc bytes before the TCP port answers / shadNet's answers not acked | 1 MiB / 9 MiB | the connection ends | `link_server.cpp` |
| Message fields: names 64 bytes, characters 96, party key 256, portrait 128x128 RGB, join data 4 KiB, map state 1250 bytes, 4096 shown characters, HP enemies, flag events and echoes, 32768 kills, 250,000 team world flags, 64 trade items, trophies, revive hits | | the message is dropped (logged) | `Service::Oversized` |
| Party worlds in memory | 1024 (hosted: `HostedPartyWorlds`); one nobody is in for 5 min goes back to disk only (where its members stood is kept for when a new hello reads it again, for a day and 4096 worlds at most); at the cap the 4 oldest such go | a hello for a new world is refused | `Service::Collect` |

Also for everyone: a summon removal touches only its party's signs, the summon broker keeps at most
4096 signs, and an address sends at most 20 crash reports, then one a minute. A host can turn the
HTTP check on for its own server with `HttpLinkedOnly=true`: the games call the HTTP ports at the
same address as their link, so a LAN or Radmin game passes.

Also for everyone: each account's virtual address (10.x.y.z, a 24-bit hash of its id that the games
compute too) is stored with it, one account each (`database.cpp`, migration 3). A new id whose
address another account has is refused, so nobody squats a known player's address; the link's
refusal starts with `address taken:` (the launcher should make a new id). Accounts that already
shared an address keep signing in, logged at start. Home servers get it too: an id sharing an
address could never link while the other was linked anyway.

Fuzzing: `test_fuzz_decoders` (in ctest) feeds the link's datagrams, game frames, our messages
(`DecodeEnvelope` -> `Service::Take`) and the TCP sessions' packets a few thousand seeded random and
mutated inputs each. With clang, `-DSHADNET_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++` builds the same
targets for libFuzzer with ASan/UBSan (`fuzz_link`, `fuzz_game_frame`, `fuzz_world`,
`fuzz_session`; see `tests/fuzz/libfuzzer_main.cpp`; run with
`ASAN_OPTIONS=detect_container_overflow=0`, protobuf and Qt are not instrumented); crashing inputs
go in `tests/fuzz/regressions/<target>/`, which the ctest replays. It found one crash, fixed for
everyone: a TCP packet's blob size past 2 GB passed `StreamExtractor`'s bounds check and read far
past the packet, from any client before signing in. Four minutes of each target since: clean.

Settings: `Hosted`, `HostedBuild`, `HostedBanList`, `HostedLinksPerAddress`,
`HostedHellosPerSecond`, `HostedNewAccountsPerHour`, `HostedDatagramsPerSecond`,
`HostedBytesPerSecond`, `HostedLobbyPlayers`, `HostedLobbyIntervalMs`, `HostedWorldsPerPlayer`,
`HostedPartyWorlds`, `HostedStats`, `HostedReports`, `HostedAdminSocket`, `HostedRetireDays`,
`HttpLinkedOnly`.

The games need no change for any of this but the link's handshake below (the emulator already says
its id in hex as its names; check the port does too).

A friend the lobby does not list (it lists 32 players), on every server: each team has a 6-letter
code (no 0/O, 1/I; `coop/team_code.h`), shown to its leader on the team screen, made again with
New code. Join by code puts the player straight into that team (the leader handing out the code is
its invite, the typing the accept, so the leader has nothing more to answer). Invite by name sends
the usual invite to the one signed-in player of that name as the lobby shows it (case aside;
several of that name: refused), who is then listed to the inviter. Wrong codes and names count
per player: past 5 in a minute every lookup waits (`TeamLobby::LookupTries`). Both are typed in
the window's text entry (the game's name dialog's) and need this build on both sides
(`TeamRequest` actions 12-14, `Team.code`).

## The link's handshake (link version 3)

On for everyone, both sides (`coop/link_auth.h`, shared by shadNet, the emulator's `net_link.h`,
`bbload` and the fuzz target; no new dependency). Server and games ship together: a game of link
version 2 gets no answer.

| What | Why |
| --- | --- |
| A hello without a valid cookie gets only a cookie back (HMAC of its source address and port and a 30 s bucket; smaller than the hello, nothing kept). The hello that echoes it signs in | a spoofed source never sees its cookie: no link, account, password hash or refusal for it, and no reflection |
| Every datagram after the hello, both ways, ends in a counter and a 16-byte HMAC-SHA256 tag (24 bytes); keys from the secret, the game's nonce, the cookie and the id, one each way | nobody off the path can inject frames, Coop messages or Rpc bytes from a player's address, or replay them |
| A bad tag or a counter seen already (a window of 1024) is dropped without a word, counted (`server link: ... dropped for a bad tag` with the counters) | |
| A link's rates (`admission.h`) count a datagram only once its tag is good; hello limits only hellos with a valid cookie | a spoofed flood from a player's address got that address banned, or used up its hellos |

The secret still crosses in the hello (after the cookie): the game's NP sign-in sends the same
secret through the Rpc stream, so a challenge-response on the link alone would hide nothing.
Someone on the path still reads it and the game data; that takes encryption (Noise, below).
Cost at 300 players (three runs each, this box shared with other builds): the link thread 12.5%
-> 16.5% of a core on average, relay p50 0.33-0.40 -> 0.41-0.74 ms (p99 13-34 -> 13-93 ms, noise
of the box), 15.2 -> 17.3 Mbit/s out. One tag costs 0.6 us for 40 bytes, 1.7 us for 300, 5.7 us
for 1200 (a relayed frame: one check, one tag).

Attacks beside 300 players with it (link version 3, hosted):

| Attack | What happened | Players |
| --- | --- | --- |
| 3000 first hellos/s from 500 addresses, never echoing the cookie (all a spoofed source can do) | 57,610 cookies sent; no sign-in, no account; server 22% CPU (the link thread) | no change |
| the same echoing the cookies (a real botnet) | as before the handshake: the sign-in threads busy (server 289% CPU) | no change (link thread 20%) |
| 30,000 datagrams/s with bad tags from a signed-in game's address | all dropped, its game not banned; link thread 29% | no change |
| 30,000 datagrams/s with good tags (the game itself) | banned within seconds, as before | no change |

## Load test

`server/loadtest/`: `bbload` runs simulated games that speak the real link from their own UDP
ports and loopback addresses: hello and sign-in, ping, the team lobby (create, invite, accept,
ready, start), the party hello, vitals twice a second, kill proposals, the map every second, and
game packets to each teammate at the rates the rig measured (enemy owner ~87 a second, follower
~8). Every packet carries its send time and number, so the receiver measures the relay's delay and
losses. `run_load.py` starts a fresh shadNet, runs the players over several bbload processes,
samples the server's CPU (and its link thread's) and memory, and can run `attack.py` beside them.

```
cmake --build build --target shadnet bbload
python3 server/loadtest/run_load.py --shadnet build/shadnet --bbload build/bbload \
    --players 300 --team-size 3 --seconds 60 --cfg Hosted=true \
    [--attack "signin-flood --rate 3000 --seconds 30 --addresses 500"]
```

Results (2026-10-23; one 4-core cloud container running the server and the players together, so
the players' processes take cores from the server; teams of 3):

| Players | Server | In game | Relay p50 / p99 | Lost | Link thread | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 30 | today | 30/30 | 0.2 / 5.7 ms | <1% | ~2% | pings stall 1.7 s while players sign in |
| 60 | today | 54/60 | 0.3 / 10 ms | 2% | ~5% | joins up to 12.5 s |
| 120 | today | 50/120 | 41 / 1577 ms | 65% | 100% | lobby updates: 216 Mbit/s out |
| 120 | hosted | 120/120 | 0.3 / 1.7 ms | 0.1% | 8% | |
| 300 | hosted | 300/300 | 0.4 / 7 ms | 0.4% | 21% | 18 Mbit/s out |
| 1000 | hosted | 1002/1002 | 0.5-0.8 / 10-18 ms | 0 | 65-70% | steady state; ~50 Mbit/s out; 38 MB of memory |

With every hardening item merged (caps, writer thread, HTTP gate, address check), 300 players: relay
0.4 / 11 ms, 0.2% lost. 1002 players arriving over a minute and playing 2 minutes: all in, link
thread 66% on average with peaks at 99%, relay p50 0.8 ms but p99 ~170 ms, 6% lost; the first
prototype build gives the same numbers in that run, so it is the box's limit, not a regression.
One process on one core tops out around 1000 players: plan for fewer per process.

At 1000 the join rush (1000 new accounts within 10 s) takes up to 30-35 s; players arriving over
minutes would not see it. "Lost" counts gaps in each sender's numbers on one map; packets the
server keeps from a player on another map (by design) are not losses. Without the FlushSome fix 1000 players saturated the
link thread (relay p99 9 s).

Attacks beside 300 players (hosted):

| Attack | What happened | Players |
| --- | --- | --- |
| 3000 hellos/s with new ids from one address, 30 s | 5 accounts made, the rest dropped | no change |
| the same from 500 addresses (a botnet) | 1181 accounts made; the sign-in threads busy (server 268% CPU) | no change (link thread 19%) |
| one signed-in game sending 30,000 datagrams/s | banned within ~5 s | no change |

## How far one machine goes

The link thread (relay, worlds, lobby) is the limit: ~65% of one core at 1000 players here. A
dedicated 4-8 core machine should hold ~1000-1500 players in one process (a guess past what was
measured). Past that, worlds go to several shadNet processes (each a share of the worlds, its own
port) with a small directory saying which process runs which world: that is where Redis (or a
Postgres table) earns its place, not before. Each world's log has one writer, its process, so no
distributed database is needed for "sync".

Storage: SQLite (accounts) and the world folders (journal + snapshots) are right for one machine.
Postgres helps only with several processes or machines; moving to it is ~3-4 weeks (thread-per-
session connections, SQLite-only SQL). Backups: `server/hosting/backup.sh` (journal before
world.pb, `sqlite3 .backup`, restic to S3/B2/Storage Box), with a restore drill in
`server/hosting/README.md`.

## Before a public launch

Server only:
- Make the account limit hold against botnets: a proof-of-work in the hello, or sign-up through
  the launcher with a server-signed token.

Game and launcher:
- Encryption (Noise, ~1.5-2 weeks): the link is authenticated now, but the secret and the game
  data still cross the internet in clear.
- ~~A fresh id when the server refuses one (virtual address collision).~~ Done: the launcher makes
  one for that server only (coop.json `server_ids`, keyed by the address as typed) and starts again.
