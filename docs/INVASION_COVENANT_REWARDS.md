# Covenant rewards for server-authoritative invasions

A winning hunter wearing **Corruption** receives one Blood Dreg (goods 704), in addition to the
existing server victory payment. Radiance, Hunter, and the blood-addled Hunter state have no
extra item payment in the original 1.09 reward lots. They retain the existing Insight/Echoes
payment. This change does not invent a covenant material for those oaths.

## Original evidence

The owner supplied Bloodborne 1.09 CUSA00900. The original, read-only game archives and their
embedded row names/values are the primary source. No modified game assets are distributed.
`docs/invasions/COVENANT_REWARDS_EVIDENCE.json` records the archive/extracted-table checksums,
native call locations, and the small relevant row facts.

| Native oath marker | GemeffectParam row | Original PvP item lots (host / guest) | Additional item |
| --- | --- | --- | --- |
| 6100 Corruption | 2108001 | 5500 / 5510 | goods 704, quantity 1 |
| 6110 Radiance / Executioner | 2107001 | 5600 / 5610 | none (category -1, item 0) |
| 6120 Hunter | 2110001 | 5700 / 5710 | none (category -1, item 0) |
| 6130 blood-addled Hunter state | native effect marker | 5800 / 5810 | none (category -1, item 0) |

The Corruption rows have one enabled slot: category 4 (goods), item 704, weight 100, quantity 1;
all other weights are zero. `item.msgbnd` FMG 10 names goods 704 **Blood Dreg**. `menu.msgbnd`
FMG 31/33 identifies the Corruption, Radiance, and Hunter runes and describes their oaths.
The native reward code checks each of the four effect IDs and selects the corresponding lot:
Corruption's guest check at virtual `0x01382B08` calls the award for lot 5510 at `0x01382B26`;
its host check at `0x013830C8` calls lot 5500 at `0x013830E9`. Both call `0x017DDC50`.
The adjoining branches select the Radiance, Hunter, and blood-addled Hunter lots above.
The ELF's load-file offset is virtual address + `0x4000`; the evidence records that distinction.
This is static native/data evidence, not a claim that this document observed runtime awards.

Original `EquipParamGoods` row 704 allows **99 held and 99 in the repository**. Its signed
16-bit `maxNum` field at row offset `0x36` and `maxRepositoryNum` at `0x48` both contain 99;
`isDeposit` at byte `0x45`, bit 4, is set. The table establishes capacity and deposit eligibility;
it does not prove that a particular native inventory call automatically moves overflow to storage.
A client must verify the actual awarded count and keep the grant pending when capacity blocks it.

For corroboration, common events 9360/9480 require effect 6100 and character type 0 or 1 before
awarding NPC lot 5520; that lot also gives one Blood Dreg. Dungeon NPC lot 5530 is a different,
chance-based drop and is not used for player-versus-player rewards.

The original rune text says the Executioners will not cooperate with Vilebloods and describes
Hunter of Hunters targets. Hostility/matchmaking is separate from this server's reward inventory:
the original Radiance/Hunter item lots are empty, so those descriptions do not imply extra loot.

## Protocol and lifecycle

The append-only protocol remains 8:

- `Vitals.covenant_effect=19` reports the equipped native marker for the defending hunter.
- `InvadeRequest.covenant_effect=3` reports the invading hunter's marker.
- Allowed values are 0, 6100, 6110, 6120 and 6130; unknown values normalize to 0.
- The invading oath is frozen at match, and the target host's oath at join, alongside the
  defeated-level snapshots. Changing a rune or the death report during a visit does not change
  an invasion's reward entitlement.
- `LobbyHello.covenant_rewards=6`, together with `invasion_rewards=5`, opts in to the extended
  reason-2 grant: one Insight, optional Echoes, optional one goods-704 Blood Dreg. Every item has
  empty name/data fields. Only the exact quantity/id is valid; other goods or duplicate items
  are rejected. Ordinary legitimate trading of Blood Dregs remains supported.

A client with only the original reward capability receives the original Insight/Echoes shape.
A pending extended grant is withheld whole when an older client reconnects; it stays durable in
its authenticated mailbox and is replayed when a covenant-capable client signs in. A capability
upgrade on the same connection also resumes it. The mailbox does not discard or partially pay
an extended grant to accommodate an old validator. The client owns applying and durably saving
its payment before acknowledging; see the server commit boundary below. Client inventory/save
atomicity is a separate implementation and must be tested in the game.

Only the existing joined victory endings pay. Cancellation, environment death of the host,
party wipe, boss, timeout, disconnect, search cancellation and pre-join death remain unpaid.
A defending reward belongs to the session host, and a target-kill reward requires
`killed_by_invader=true`. Guests do not inherit either hunter's covenant entitlement.

## Sources and authors

- FromSoftware: original Bloodborne 1.09 game code, `gameparam.parambnd.dcx` tables,
  `common.emevd.dcx`, and English game FMGs supplied read-only by the owner.
- Kaniel / [lKaniel](https://github.com/lKaniel), Vitalii Shevchuk and shadNet Project
  contributors: [pinned invasion/trade/world-store foundation](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/server).
- Soulsmods/Paramdex contributors (EvanDeadlySins, GiveMeThePowa, katalash, Pavuk, thefifthmatt,
  TKGP/JKAnderson, Vawser): [ItemLotParam fields](https://github.com/soulsmods/Paramdex/blob/master/BB/Defs/ItemLotParam.xml),
  [GemeffectParam fields](https://github.com/soulsmods/Paramdex/blob/master/BB/Defs/GemeffectParam.xml),
  [EquipParamGoods fields](https://github.com/soulsmods/Paramdex/blob/master/BB/Defs/EquipParamGoods.xml).
  Their [README](https://github.com/soulsmods/Paramdex#readme) identifies the Bloodborne paramdefs
  as originally shipped with the game and lists the contributors.
- Grimrukh: [Soulstruct's Bloodborne EMEVD definitions](https://github.com/Grimrukh/soulstruct/blob/main/src/soulstruct/bloodborne/events/emevd/emedf.py),
  consulted to corroborate event instruction aliases; the actual reward rows/calls above come
  from the supplied game's original assets and native code.

## Durable server commit boundary

A new reward and its next grant counter are staged in a copy of the confirmed mailbox. A
synchronous completion task enters the same FIFO `WorldWriter` queue as older asynchronous
snapshots, so none of those snapshots can overwrite a newer confirmed reward or ACK afterward.
The writer checks the full write, explicitly flushes and fsyncs the temporary file, commits its
atomic rename, then fsyncs the containing directory and ancestors (including newly initialized
mailbox parents). Direct overwrite fallback remains disabled. Only a successful completion
publishes the new mailbox state and queues the grant to the client. ACK removal follows the
same boundary, retaining the prior confirmed in-memory grant on failure.

The existing authenticated `trades.pb` ledger is sufficient to load a mailbox after restart;
`world.pb` is not a prerequisite. A present unreadable ledger, including an invalid zero/high-bit
counter, fails closed instead of becoming a fresh counter. A failed new payment sends nothing
and keeps the frozen invasion entitlement active for a later dead-Vitals retry, with a one-second
failure backoff. In-memory-only server configurations have no disk durability guarantee.

Focused tests check durable bytes during the actual grant-delivery callback, before any Drain;
process-stop/restart with no world file; queued older snapshots before a durable ACK; failures
before file sync, before rename, after rename, and after directory sync; failed grant publication;
failed ACK retention/retry; and corrupt-ledger preservation. These are controlled process/fault
fixtures, not physical power-cut tests. Durability depends on the filesystem/storage honoring
successful flush calls. The complete reward implementation passed the ten focused suites on
Linux ARM64 and native Windows Server 2022 with MSVC and Qt 6.8.3. The
[Windows validation run](https://github.com/evill33t/bloodborne-server/actions/runs/40559939580)
includes the restart, durable delivery, ordered ACK and write-failure fixtures.

On Windows, directory handles use `FILE_FLAG_BACKUP_SEMANTICS` and `GENERIC_WRITE`.
A native probe reproduced `ERROR_ACCESS_DENIED` when flushing a read-only handle and successful
flushes with write access. File and ancestor-directory open/flush failures remain failures:
the server publishes no new reward or ACK state if filesystem support or access rights do not
permit the required flushes. The Windows run validates API/process behavior on that runner;
it does not certify physical power-loss behavior or every filesystem and permissions policy.
The access requirement follows Microsoft's
[FlushFileBuffers documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
and [CreateFile directory-handle flags](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
(Microsoft and Microsoft Learn contributors).

The distinction follows [Qt's QSaveFile atomic replacement contract](https://doc.qt.io/qt-6/qsavefile.html)
and the [Linux man-pages fsync directory requirement](https://man7.org/linux/man-pages/man2/fsync.2.html)
(Linux man-pages project contributors; Michael Kerrisk). Original Kaniel/lKaniel, Vitalii Shevchuk
and shadNet source attribution remains above and in every implementation commit.
