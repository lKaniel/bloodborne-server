# Trading between party members

Party members give each other Blood Echoes and any item (Kaniel 2026-10-15): goods of every kind
(key items too), weapons with their upgrades and the blood gems in them, armor, loose gems and
runes. They may ask for echoes or goods back (a swap); not for a weapon, armor or gem (the asked
side could not say which one). The bell items stay out (the port's game removes them anyway). The server owns every trade; each game owns its own inventory. Always on in
the port with the server world (Kaniel 2026-10-14); shadPS4 has none.

## How a trade goes

1. The giver picks "Give" on any item in the inventory, or "Give Blood Echoes" (on every item's
   commands while it holds any echoes and someone is near), and a number in the game's quantity
   picker (cursor on No; down from 1 wraps to all of them). "Give Blood Echoes" is the last row,
   one Up from the first. For echoes the picker says to whom and how many the giver holds ("Give
   Blood Echoes to Ada (you hold 54,321).\nUp/Down: 1,000 at a time, up to all of them."): Up/Down
   step by a round amount under a hundredth of that (1 under 100), the picker opens on one step,
   its lowest, and past the last step (or down from the first) comes all of them. The offer goes to the nearest other party member
   standing on the same map. The game's inventory hides vials and bullets (they are on the HUD
   only); the port lists them in the Consumables tab (row filter 0x01BD8150 made
   to keep every row), with the game's own Use and Discard too.
2. The giver's game takes the items out of its inventory at once, then sends `TradeOffer`.
3. shadNet checks it (both members online in the party, tradeable items, counts within limits, at
   most 4 open offers per member) and asks the other member (`TradeAsk`), or gives valid ordinary
   items straight back (refused). Invalid or unknown item claims are never turned into grants.
4. The other member's game shows the game's yes/no box: "Ada offers you 4 Blood Vials. Accept?"
   (the cursor starts on No). To accept a swap its game takes out what was asked of it first.
5. shadNet hands each side its part (`TradeGrant`); declined, unanswered for 60 s, or withdrawn,
   the offer goes back to the giver. A game adds a grant like a pickup (up to the max held, the
   rest to the storage box, with the game's own "item obtained" popup), adds echoes through
   AddSouls, and acknowledges it (`TradeGranted`).
6. The giver sees what became of its offer in the game's OK box.

shadNet keeps open offers and unacknowledged grants on disk (`trades.pb` in the party's world
folder, written whole on every change) and hands them again when a member's game says hello, so a
crash or a restart of either side loses nothing. A grant is added once per game run.

Known limits: a giver whose game crashes after taking items out but before its offer reached the
server (one stream tick, 20 ms) loses them if the game saved in between. The goods' picker steps
one at a time (99 at most). An offer's box opens in the world
whenever no menu is open, also mid-fight; its cursor starts on No.

## Code

- `server/coop/trade.{h,cpp}`: the shared part. `TradeDesk` (shadNet's per-party trades),
  `TradeClient` (a game's offers, asks and grants), the tradeable items and the game's words.
- `server/coop/coop.proto`: `TradeOffer`, `TradeStatus`, `TradeAsk`, `TradeAnswer`, `TradeCancel`,
  `TradeClosed`, `TradeGrant`, `TradeGranted`, `StoredTrades`; `Welcome.trades`.
- `server/src/bloodborne_world.cpp`: the desk per party world, its file, expiry on the tick.
- `emulator/src/core/bloodborne_trade.cpp`: the inventory (take out, put in), the client.
- `bbport/gpu/shim/bbport_trade_menu.cpp` + `bbport/gpu/bbcoop/trade.cpp`: the menus.
- Rig: scenarios `trade` and `trade-echoes` (headless) and `trade-menus` (drawn, screenshots); verbs `trade-stock`,
  `trade-give`, `trade-offer`, `trade-answer`, `expect-held`, `wait-trade`.

## Items of their own (weapons, armor, gems; static RE 2026-10-15)

`TradeItem` kind 3 carries the full item id (category | id: weapon 0x0, armor 0x1, accessory 0x2,
gem 0x8), one each, its name in the giver's game (`name`, from the game's own text: item.msgbnd FMG
10/11/12, menu.msgbnd FMG 31 for gems and runes) and what makes it itself (`data`).

| What | Where |
|---|---|
| SprjGaitem (the items' objects) | `[0x0553E990]`: object at +8 + (handle & 0xFFFF) * 8 (+8 handle, +0xC item id), references at +0x800C + index * 8; handles with bit 0x800000 |
| Make one | `0x01A86FF0(u32* out, SprjGaitem, const u32* item id)`; let go `0x01A8BC00(u32*)` |
| Write / read it whole | vtable +0x40 / +0x48 (stream), stream vtable +0x20 = (self, bytes, size): a weapon its fields +0x18, +0x1C, +0x28 and five gem slots (shape, handle, a gem in it whole: id, +0x18, +0x1C, six effects); the read makes the gems anew |
| Take one out | `0x014D0310(EquipGameData, index, 1)`: unequips it (15 equipment slots, quick items), then the entry goes (the inventory's Discard) |
| Add one | as a pickup, `0x017D89F0` with {handle of a new object, item id, 1, -1} |
| The row's item | the item commands' factory +8 is the item's record: +0x4C item id, +0x70 its handle |

The giver writes the item into `data` before taking it out; the receiver makes a new object, reads
`data` into it and adds it like a pickup (the game's "item obtained" popup). Rig: scenario `trade-any`
(a key item, a Hunter Axe +5 with a gem in it, armor), verbs `goods:<id>`, `weapon:<id>`,
`armor:<id>`, `gem:<id>`, `trade-socket`, `expect-socketed`.

## The game's functions (1.09, static RE 2026-10-14)

| What | Where |
|---|---|
| PlayerGameData | `[0x0553B130] + 0x08`; +0x94 Blood Echoes, +0x328 inventory, +0x1D0 EquipGameData |
| Find an item's inventory index | `0x014D9F60(inventory, &item id)`; entries 16 bytes {handle, item id, count} |
| Change a count | `0x014D0BE0(EquipGameData, index, delta, force, no_replace, allow 111)`; clamp first: below 0 turns into the max |
| Give like a pickup | `0x017D89F0(MapItemMan [0x0553D6E0], {n, {handle 0xB0000000\|id, item 0x40000000\|id, count, -1}[n]})` |
| Add echoes | AddSouls `0x018FC600` (player vtable +0x3D0) |
| Item commands list | `call 0x01BEA630` at `0x01BA152A` (factory vtable 0x0533FE00, item id at factory +8 +0x4C) |
| Add a row | `0x01B4C9A0(model, name+help texts, Function*, 0)` |
| A picked row runs | the commands' select `0x01BA2600` runs it (`0x01BEAAD0`) only when the game allows: Discard unless `0x014D0430(EquipGameData, inventory index)` says 3 (the player can't change equipment now), every other row only when `0x016E4F00(inventory index)` says the item can be used now: never for a weapon, armor, gem or rune, so their Give rows did nothing (play test 2026-10-18). The port calls its own check at `0x01BA2D81` (the picked row's label at rbp-0xA8): its Give rows run as Discard does, Give Blood Echoes always (`bbport_trade_rows.h`) |
| Quantity picker | `0x01C203C0(Job**, owner, L"NumSelect", text, first, max, on_ok, extra)`: `first` is the number it opens on, the lowest is always 1 |
| Picker's numbers | stored at `0x01C2040E` as {lowest 1, max, first, step 1, other step 10}; input `0x01ADD210`: Up/Down (menu inputs 0x24/0x20) the step, clamped, wrapping at the ends; the other step on inputs 0x3C/0x3D and 0x42/0x43 (not L1/R1, L2/R2 or Left/Right: the rig found no effect; Left/Right move between Yes and No). The port calls a stub there that takes the lowest and the steps from its own (the game's for the game's pickers) |
| Yes/no, OK box | `0x01C1ED10`, `0x01C1F500` (Job**, owner, text); answer MenuMan `[0x05562878]` +0x1CC (1 yes, 2 no) |
| Show in the world | root `0x0176BAB0()`, owner root+0x48, push `0x01BB27A0(Handle*, root, Job**, abort)`; not while MenuMan +0x2FC; the box is gone once ours is the job's last reference |
| Inventory row filter | `0x01BD8150` leaves out goods 900, 901, 1000; the port makes it `mov al, 1; ret` |

Goods: Blood Vial 1000, Quicksilver Bullets 900 (max held 20, storage 600). Other consumables that
could follow: Molotov 1200, Throwing Knife 1240, Pebble 1230, Antidote 1100, Sedative 1101,
Numbing Mist 1270, Blood Stone Shard 3000, Twin Shards 3010, Chunk 3020, Blood Rock 3030.

## Server-issued invasion grants

Opt-in invasion victory grants reuse the durable grant/ack path with reason 2, high-bit grant
IDs and server-only Insight kind 4. They are never ordinary trades or refunds. Their per-player
authenticated mailboxes, compatibility gate and safety limits are described in [INVASIONS.md](INVASIONS.md#opt-in-victory-rewards-2026-10-27).
