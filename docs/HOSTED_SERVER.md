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
| `banned.txt`: one address or player id a line, read every 30 s | `link_server.cpp` | the operator's bans |
| `HostedBuild`: the build every game must say | `server_link.cpp` | otherwise the first game to link decides it |
| Lobby and party names must be the player's own id (hex, as the games say them) | `bloodborne_world.cpp` `OwnName` | anyone could act in the lobby, or say hello into a party, as another player |
| A party hello must be for the world the player's team plays | `bloodborne_world.cpp` `OnHello` | any player could join any world by its key |
| A world is its players' (`TeamWorldInfo.members`): listed to them only, forgotten only by them (each for itself) | `team_lobby.cpp` | every world was listed to everyone; anyone could forget anyone's; 16 worlds server-wide |
| The HTTP ports (20443 the game's services, 31315 summons and WebAPI) answer only addresses with a signed-in link, and only as a player linked from there (the backend's `UserId` or `PlatformAccountId`, the summon's `UserId`, the bearer token's account); 403 otherwise. GETs (ss.info, status) act as nobody and stay open | `http_gate.h` | the player id came from the request: anyone reaching the ports could act as any player |
| Crash reports (`/bb/report`) off unless `HostedReports` (then 8 MB each); the stats page off unless `HostedStats` | `webapi_server.cpp`, `server.cpp` | a report costs a password hash; the stats page lists player ids |
| The lobby goes out at most every 250 ms, only to players whose own state changed, listing their team, invites and 32 others | `team_lobby.cpp` `StateFor`, `bloodborne_world.cpp` `SendLobby` | every lobby change sent every player the whole server's list: O(n^2), 216 Mbit/s at 120 players |
| A trade only between members of one started team | `bloodborne_world.cpp` `TakeTrade` | trades go by member name; a stranger in a party could be offered goods or answer for another |
| Party worlds in memory: at most `HostedPartyWorlds` (2048) | `bloodborne_world.h` `Limits` | every team's world on one machine |

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
| Party worlds in memory | 1024 (hosted: `HostedPartyWorlds`); one nobody is in for 5 min goes back to disk only, and a new hello reads it again | a hello for a new world is refused | `Service::Collect` |

Also for everyone: a summon removal touches only its party's signs, the summon broker keeps at most
4096 signs, and an address sends at most 20 crash reports, then one a minute. A host can turn the
HTTP check on for its own server with `HttpLinkedOnly=true`: the games call the HTTP ports at the
same address as their link, so a LAN or Radmin game passes.

Settings: `Hosted`, `HostedBuild`, `HostedBanList`, `HostedLinksPerAddress`,
`HostedHellosPerSecond`, `HostedNewAccountsPerHour`, `HostedDatagramsPerSecond`,
`HostedBytesPerSecond`, `HostedLobbyPlayers`, `HostedLobbyIntervalMs`, `HostedWorldsPerPlayer`,
`HostedPartyWorlds`, `HostedStats`, `HostedReports`,
`HttpLinkedOnly`.

Invasions (a solo player invading a team world whose leader allows it) are off unless
`Invasions=true`, hosted or at home; the server side, its settings and what the games still need:
[INVASIONS.md](INVASIONS.md).

The games need no change for any of this (the emulator already says its id in hex as its names;
check the port does too). One game-side change is wanted: on a hosted server the lobby lists 32
players, so inviting a friend needs invite by name or code.

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
- Store each account's virtual address and refuse a colliding new account (24-bit hash).
- Make the account limit hold against botnets: a proof-of-work in the hello, or sign-up through
  the launcher with a server-signed token.
- Retire worlds nobody played for months (disk).

Game and launcher:
- A cookie handshake and an HMAC on every datagram (secrets cross the internet in clear today, and
  after the hello a datagram is trusted by its address), ~3-5 days; or Noise, ~1.5-2 weeks.
- Invite by name or code (the lobby lists 32 players).
- A fresh id when the server refuses one (virtual address collision).
