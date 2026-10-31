#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The party HUD: the game's own HUD movie (menu/fe.gfx) with party death's downed banner, made at every start
from the player's copy (or the mod that replaces it, such as Compact HUD) and put over the mods as the last
layer. Nothing of the game's is kept in the port: the movie is read and patched where it is.

The banner is the game's TextEffect_YouWin (UNLOCKED, SetTextEffect id 15): its text fields are renamed, so that
the game no longer fills them from its text tables (fields named StaticText_<id>), and get our words in the
game's font, in yellow. The game's fade-in and blur trail stay as they are. It has the same timeline and layers
as TextEffect_Draw, which stays the game's NIGHTMARE SLAIN (text 522007, SetTextEffect id 17: Mergo's Wet Nurse,
Gehrman, the Orphan of Kos; player report 2026-10-29). Only one event shows UNLOCKED, m29 12900192, and the event
patch drops its banner (tools/re/party_boss_events.py DROPPED_BANNER_EVENTS).

The party list is three more of the game's own nameplate tags (the sprite RemotePCTag0 shows), placed at the
top level as CoopParty0-2: party death fills and places them each frame through the game's own tag code
(bloodborne_party_death.cpp), so the list has the game's font, bars and fades and sits under its menus. Left of
each sits CoopPartyIcon0-2, a sprite whose frames are our own icon drawings (scripts/party_hud_icons.bin, made by
party_icons_atlas.py; HUD_ICON_FRAMES): party death shows the member's with GotoAndStop.

A member whose portrait came (its face as its own game renders it on the status page, sent to the party) shows
it in CoopPartyFace0-2 instead, left of the whole block, with the icon as a small badge on its top right corner
(CoopPartyBadge0-2, the icon sprite again). Each face is a sprite of two frames (nothing, then a square of a
bitmap of its own under the portrait frame): the bitmap is one placeholder colour (portrait_magic), by which the
port's texture cache knows the slot's texture and puts the member's picture in its place, round
(bbport_portraits.h, CircularPortrait); the frame is a ring drawn with the icons (h-frame, the strip's last
drawing), shown and hidden with the picture."""
import hashlib
import zlib
from pathlib import Path
import struct

FE = 'dvdroot_ps4/menu/fe.gfx'
LAYER = 'party HUD'
# Part of the generated file's name: a patcher that patches differently makes its file anew (the old
# one is not reused). Bump it with every change to what patch_fe writes.
PATCH_VERSION = b'party-hud 16'
DOWNED_BANNER = 'Fe_fla._TextEffect_YouWin_181'
DOWNED_SOURCE_FIELD = 'StaticText_522002'
DOWNED_FIELD = 'CoopDownedText'
DOWNED_TEXT = 'YOU ARE DOWN'
DOWNED_COLOR = 'd8b03c'

PARTY_TAG = 'RemotePCTag0'
PARTY_LIST = ('CoopParty0', 'CoopParty1', 'CoopParty2')
PARTY_ICONS = ('CoopPartyIcon0', 'CoopPartyIcon1', 'CoopPartyIcon2')
# Party death's prompt: the game's interact prompt (the sprite ActionButtonTop shows) once more, a little above it
# (PROMPT_AT on the 1920x1080 stage; its button glyph's centre is PromptGlyphStage in bbport_party_layout.h).
PROMPT_TAG, PROMPT, PROMPT_AT = 'ActionButtonTop', 'CoopPrompt', (952.6, 808.0)
HUD_ICONS = Path(__file__).resolve().parent / 'party_hud_icons.bin'
# The icon sprite's frames (1-based, as GotoAndStop takes them; PartyListIconFrame in bloodborne_party.cpp).
HUD_ICON_FRAMES = ('none', 'hunter', 'elsewhere', 'fallen') + tuple(f'downed {p}%' for p in range(0, 101, 10))
# Where the list's tags stand on the HUD's 1920x1080 stage (PartyListPosition in bloodborne_party.cpp).
LIST_X, LIST_Y, LIST_STEP = 204, 330, 95  # each bar starts where the player's HP bar does (x 61)
# Where a tag's HP bar starts and its middle runs, from the tag (measured on a 1080p rig screenshot, 2026-10-13:
# x 109, y 329.5 for the first tag; the 2026-10-12 measure put the icon 75 px left of it and 17 px below, Kaniel;
# 8 px of padding then looked too tight to him).
# The icon (its size in pixels) goes ICON_PAD px left of the bar, centred on it.
BAR_FROM_TAG = (-95, -0.5)
ICON_SIZE, ICON_PAD = 36, 18
# The member block (its name over the HP bar), from the bar's middle (the same 1080p screenshot): the name's top
# and the bar's bottom.
NAME_TOP_FROM_BAR, BAR_BOTTOM_FROM_BAR = -34.5, 4.5
# The portraits (Kaniel 2026-10-13: 1.5-2x the icon, centred on the whole block, the icon a small badge at its
# top right; then round, in a Bloodborne frame; after the 3-player playtest 1.5-2x smaller, 64 to 36 px; 2026-10-17
# "really poor quality": bitmaps of 128 px, sharp at 4K, where the HUD shows 72 px; the same day the co-op art's
# wrought-iron frame, whose inner gold ring is 69% of its width: the frame 52 px round a 36 px portrait): their
# size on the stage, their bitmaps' side (the port's PortraitSide), the frame's size on the stage and its
# drawing's side (drawn larger, crisp when scaled; the ring keeps the picture's 4:5 to the frame), the frame
# ICON_PAD px left of the bar; the badge's size, its centre BADGE_DISTANCE px up and right of the portrait's, on
# the diagonal, over the ring.
PARTY_FACES = ('CoopPartyFace0', 'CoopPartyFace1', 'CoopPartyFace2')
# Kaniel 2026-10-18: each member's Blood Vials and Quicksilver Bullets on its name's line, with the HUD's own
# vial and bullet (the player's TriDrug and AmmoItem: their icon drawings and their count's text field, without
# the square slot): CoopSupply0-2, a sprite of two frames (the counts, empty while nobody shows; then the icons
# too), its text fields named SupplyVials and SupplyBullets (party death writes the counts). Its layout, from
# the sprite's origin at the name line's right part: each icon SUPPLY_ICON px tall, its count right of it.
PARTY_SUPPLIES = ('CoopSupply0', 'CoopSupply1', 'CoopSupply2')
SUPPLY_TEXTS = ('SupplyVials', 'SupplyBullets')
SUPPLY_ICON, SUPPLY_TEXT_SCALE, SUPPLY_STEP = 22, 0.75, 48  # icon px, the count's scale, from vial to bullet
SUPPLY_X, SUPPLY_FROM_BAR = 212, -20  # the vial's centre x on the stage; the line's y from the bar's middle
# Kaniel 2026-10-18 ("will it work fine with a long name?"): they follow the name. Frame 2 shows them at SUPPLY_X,
# each next frame SUPPLY_SHIFT px further right; party death picks the frame by the name's width
# (bloodborne_party.h SupplyFrame), up to what a 16-character name needs.
SUPPLY_SHIFT, SUPPLY_SHIFTS = 6.0, 32
FIRST_SUPPLY_ID = 50120  # after the faces' ids
PARTY_BADGES = ('CoopPartyBadge0', 'CoopPartyBadge1', 'CoopPartyBadge2')
PORTRAIT_SIZE, PORTRAIT_PIXELS = 42, 128
FRAME_SIZE, FRAME_PIXELS = 61, 128
BADGE_SIZE, BADGE_DISTANCE = 18, 29
FIRST_FACE_ID = 50100  # after the icons' ids


def portrait_magic(slot):
    """(r, g, b, a) of slot's placeholder bitmap: red and blue alike, so the same in RGBA and BGRA
    (bbport_portraits.h PlaceholderSlot)."""
    return (0x3D, 0xC0 + slot, 0x3D, 0xFF)


def icon_centre(index):
    """The centre of CoopPartyIcon<index> on the stage."""
    return (LIST_X + BAR_FROM_TAG[0] - ICON_PAD - ICON_SIZE / 2,
            LIST_Y + LIST_STEP * index + BAR_FROM_TAG[1])


def portrait_centre(index):
    """The centre of CoopPartyFace<index> (and its frame) on the stage: left of the bar as the icon, on the whole
    block."""
    bar_y = LIST_Y + LIST_STEP * index + BAR_FROM_TAG[1]
    return (LIST_X + BAR_FROM_TAG[0] - ICON_PAD - FRAME_SIZE / 2,
            bar_y + (NAME_TOP_FROM_BAR + BAR_BOTTOM_FROM_BAR) / 2)


def badge_centre(index):
    """The centre of CoopPartyBadge<index>: up and right of the portrait's, over its frame's ring."""
    x, y = portrait_centre(index)
    step = BADGE_DISTANCE / 2 ** 0.5
    return x + step, y - step
# Character ids for what the patch defines, above any the game's movies use.
FIRST_NEW_ID = 50000

SHOW_FRAME, PLACE_OBJECT2, PLACE_OBJECT3, EDIT_TEXT, SPRITE, SYMBOL_CLASS = 1, 26, 70, 37, 39, 76
REMOVE_OBJECT2 = 28


class Tag:
    """One tag: its code and body; a sprite's body is its header and its own tags."""

    def __init__(self, code, body, long):
        self.code, self.body, self.long = code, body, long
        self.children = None
        if code == SPRITE:
            self.children = read_tags(body, 4)
            self.body = body[:4]

    def encode(self):
        body = self.body + (b''.join(child.encode() for child in self.children) if self.children is not None else b'')
        if len(body) < 0x3F and not self.long:
            return struct.pack('<H', self.code << 6 | len(body)) + body
        return struct.pack('<HI', self.code << 6 | 0x3F, len(body)) + body


def read_tags(data, offset):
    tags = []
    while offset < len(data):
        header, = struct.unpack_from('<H', data, offset)
        offset += 2
        code, length, long = header >> 6, header & 0x3F, False
        if length == 0x3F:
            length, = struct.unpack_from('<I', data, offset)
            offset, long = offset + 4, True
        if offset + length > len(data):
            raise ValueError('a tag runs past the end of the movie')
        tags.append(Tag(code, data[offset:offset + length], long))
        offset += length
        if code == 0:
            break
    return tags


def rect_size(data, offset):
    bits = data[offset] >> 3
    return (5 + 4 * bits + 7) // 8


def read_movie(data):
    """(header, tags) of an uncompressed GFX/SWF movie."""
    if data[:3] not in (b'GFX', b'FWS') or len(data) < 12:
        raise ValueError('not an uncompressed GFX movie')
    if struct.unpack_from('<I', data, 4)[0] != len(data):
        raise ValueError('the movie\'s length does not match its file')
    start = 8 + rect_size(data, 8) + 4
    return data[:start], read_tags(data, start)


def write_movie(movie):
    header, tags = movie
    body = header[8:] + b''.join(tag.encode() for tag in tags)
    return header[:4] + struct.pack('<I', 8 + len(body)) + body


class Bits:
    def __init__(self, data, offset):
        self.data, self.offset, self.bit = data, offset, 0

    def read(self, count):
        value = 0
        for _ in range(count):
            byte = self.data[self.offset]
            value = value << 1 | (byte >> (7 - self.bit)) & 1
            self.bit += 1
            if self.bit == 8:
                self.offset, self.bit = self.offset + 1, 0
        return value

    def end(self):
        return self.offset + (1 if self.bit else 0)


def skip_matrix(data, offset):
    bits = Bits(data, offset)
    for _ in range(2):  # scale, rotate/skew
        if bits.read(1):
            bits.read(2 * bits.read(5))
    bits.read(2 * bits.read(5))  # translate
    return bits.end()


def skip_cxform(data, offset):
    bits = Bits(data, offset)
    add, mult, size = bits.read(1), bits.read(1), bits.read(4)
    bits.read(size * 4 * (add + mult))
    return bits.end()


def place_parts(tag):
    """(character or None, name or None, (start, end) of the name's bytes) of a PlaceObject2/3."""
    body = tag.body
    flags, offset = body[0], 1
    flags2 = 0
    if tag.code == PLACE_OBJECT3:
        flags2, offset = body[1], 2
    offset += 2  # depth
    if tag.code == PLACE_OBJECT3 and (flags2 & 0x08 or (flags2 & 0x10 and flags & 0x02)):
        offset = body.index(b'\0', offset) + 1  # class name
    character = None
    if flags & 0x02:
        character, = struct.unpack_from('<H', body, offset)
        offset += 2
    if flags & 0x04:
        offset = skip_matrix(body, offset)
    if flags & 0x08:
        offset = skip_cxform(body, offset)
    if flags & 0x10:
        offset += 2
    if not flags & 0x20:
        return character, None, None
    end = body.index(b'\0', offset)
    return character, body[offset:end].decode('latin-1'), (offset, end)


def rename(tag, name):
    _, _, (start, end) = place_parts(tag)
    tag.body = tag.body[:start] + name.encode('latin-1') + tag.body[end:]


def edit_text_parts(tag):
    """{field: (start, end)} of a DefineEditText's colour and initial text."""
    body = tag.body
    offset = 2
    offset += rect_size(body, offset)
    first, second = body[offset], body[offset + 1]
    offset += 2
    parts = {}
    if first & 0x01:
        offset += 2  # font id
    if second & 0x80:
        offset = body.index(b'\0', offset) + 1  # font class
    if first & 0x01 or second & 0x80:
        offset += 2  # height
    if first & 0x04:
        parts['color'] = (offset, offset + 4)
        offset += 4
    if first & 0x02:
        offset += 2
    if second & 0x20:
        offset += 9
    offset = body.index(b'\0', offset) + 1  # variable name
    if first & 0x80:
        parts['text'] = (offset, body.index(b'\0', offset))
    return parts


def downed_html(html):
    """The banner's html with our words and colour, its layout kept."""
    head, bold, rest = html.partition('<b>')
    _, close, tail = rest.partition('</b>')
    if not bold or not close or 'color="#' not in head:
        raise ValueError("the banner's text is not the html it should be")
    color = head.index('color="#') + len('color="#')
    head = head[:color] + DOWNED_COLOR + head[color + 6:]
    return f'{head}<b>{DOWNED_TEXT}</b>{tail}'


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, count, signed=False):
        if signed and value < 0:
            value += 1 << count
        self.bits += [(value >> (count - 1 - i)) & 1 for i in range(count)]

    def data(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def signed_bits(*values):
    return max(max(abs(v) for v in values).bit_length() + 1, 1)


def matrix(scale, tx, ty):
    """A MATRIX: scale (16.16) and translation (twips), no rotation."""
    bits = BitWriter()
    if scale is None:
        bits.put(0, 1)
    else:
        fixed = round(scale * 65536)
        n = signed_bits(fixed)
        bits.put(1, 1)
        bits.put(n, 5)
        bits.put(fixed, n, True)
        bits.put(fixed, n, True)
    bits.put(0, 1)
    n = signed_bits(tx, ty)
    bits.put(n, 5)
    bits.put(tx, n, True)
    bits.put(ty, n, True)
    return bits.data()


def rect(width, height):
    n = signed_bits(width, height)
    bits = BitWriter()
    bits.put(n, 5)
    for value in (0, width, 0, height):
        bits.put(value, n, True)
    return bits.data()


def read_strip(data):
    """(count, [RGBA bytes of each 128-pixel icon]) of a party_icons_atlas.py strip."""
    if data[:4] != b'BBIC':
        raise ValueError('not a party icon strip')
    width, height, count = struct.unpack_from('<HHH', data, 4)
    rgba = zlib.decompress(data[10:])
    if height != 128 or width != 128 * count or len(rgba) != width * height * 4:
        raise ValueError('the party icon strip is not a row of 128-pixel icons')
    icons = []
    for index in range(count):
        icon = bytearray()
        for y in range(128):
            start = (y * width + index * 128) * 4
            icon += rgba[start:start + 128 * 4]
        icons.append(bytes(icon))
    return count, icons


def bitmap(char, rgba, pixels=128):
    """DefineBitsLossless2: `pixels` square, premultiplied ARGB."""
    argb = bytearray(len(rgba))
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i:i + 4]
        argb[i:i + 4] = bytes((a, r * a // 255, g * a // 255, b * a // 255))
    return Tag(36, struct.pack('<HBHH', char, 5, pixels, pixels) + zlib.compress(bytes(argb), 9), True)


def bitmap_square(char, bitmap_char, pixels=128):
    """DefineShape: a `pixels`-pixel square filled with the bitmap (clipped), its top left at the origin."""
    side = pixels * 20
    fill = bytes([1, 0x41]) + struct.pack('<H', bitmap_char) + matrix(20.0, 0, 0)
    bits = BitWriter()
    bits.put(1, 4)  # fill index bits
    bits.put(0, 4)  # line index bits
    # Style change: fill style 1, move to (0, 0).
    bits.put(0, 1); bits.put(0, 1); bits.put(0, 1); bits.put(1, 1); bits.put(0, 1); bits.put(1, 1)
    bits.put(1, 5); bits.put(0, 1, True); bits.put(0, 1, True)
    bits.put(1, 1)
    n = signed_bits(side)
    for dx, dy in ((side, 0), (0, side), (-side, 0), (0, -side)):
        bits.put(1, 1); bits.put(1, 1); bits.put(n - 2, 4); bits.put(0, 1)
        bits.put(1 if dx == 0 else 0, 1)
        bits.put(dy if dx == 0 else dx, n, True)
    bits.put(0, 6)  # end of shape
    return Tag(2, struct.pack('<H', char) + rect(side, side) + fill + b'\0' + bits.data(), False)


def shape_bitmap(shape):
    """The bitmap a bitmap_square shape is filled with."""
    offset = 2 + rect_size(shape.body, 2)
    return struct.unpack_from('<H', shape.body, offset + 2)[0]


def place_matrix(tag):
    """(scale or None, tx, ty in twips) of a PlaceObject2's matrix (no rotation), None without one."""
    body = tag.body
    if not body[0] & 0x04:
        return None
    offset = 3 + (2 if body[0] & 0x02 else 0)
    bits = Bits(body, offset)

    def signed(count):
        value = bits.read(count)
        return value - (1 << count) if count and value >> (count - 1) else value
    scale = None
    if bits.read(1):
        n = bits.read(5)
        scale = signed(n) / 65536
        signed(n)
    if bits.read(1):
        bits.read(2 * bits.read(5))
    n = bits.read(5)
    return scale, signed(n), signed(n)


def place(char, depth, name=None, place_matrix=None, replace=False):
    flags = 0x02 | (0x01 if replace else 0) | (0x04 if place_matrix else 0) | (0x20 if name else 0)
    body = bytes([flags]) + struct.pack('<HH', depth, char) + (place_matrix or b'')
    return Tag(PLACE_OBJECT2, body + (name.encode('latin-1') + b'\0' if name else b''), False)


def icon_sprite(char, shapes):
    """A sprite showing nothing in its first frame, then each shape in a frame of its own."""
    frames = [Tag(SHOW_FRAME, b'', False)]
    for index, shape in enumerate(shapes):
        frames.append(place(shape, 1, replace=index > 0))
        frames.append(Tag(SHOW_FRAME, b'', False))
    sprite = Tag(SPRITE, struct.pack('<HH', char, len(shapes) + 1), True)
    sprite.children = frames + [Tag(0, b'', False)]
    return sprite


def strip_drawings(strip):
    """(the icons, as HUD_ICON_FRAMES lists them, and the portrait frame) of the HUD strip."""
    count, icons = read_strip(strip)
    if count != len(HUD_ICON_FRAMES) + 1:
        raise ValueError(f'the party icon strip has {count} drawings, not {len(HUD_ICON_FRAMES) + 1}')
    return icons[:-1], icons[-1]


def party_icon_tags(depth, strip):
    """The tags that define the icon sprite and place CoopPartyIcon0-2 (from `depth` up)."""
    icons, _ = strip_drawings(strip)
    tags, shapes, char = [], [], FIRST_NEW_ID
    for rgba in icons[1:]:  # the first frame shows nothing
        tags += [bitmap(char, rgba), bitmap_square(char + 1, char)]
        shapes.append(char + 1)
        char += 2
    tags.append(icon_sprite(char, shapes))
    scale = ICON_SIZE / 128
    for index, name in enumerate(PARTY_ICONS):
        depth += 2
        cx, cy = icon_centre(index)
        tags.append(place(char, depth, name, matrix(scale, round((cx - ICON_SIZE / 2) * 20),
                                                    round((cy - ICON_SIZE / 2) * 20))))
    return tags


# The downed ring's red (the party list's downed drop's).
DOWNED_RED = (208, 40, 30)


def red_ring(rgba):
    """The portrait ring (RGBA) in red: each pixel the red at its own brightness, its coverage kept."""
    out = bytearray(len(rgba))
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i:i + 4]
        light = min(1.0, (0.299 * r + 0.587 * g + 0.114 * b) / 170.0)
        out[i:i + 4] = bytes((round(DOWNED_RED[0] * light), round(DOWNED_RED[1] * light), round(DOWNED_RED[2] * light), a))
    return bytes(out)


def face_sprite(char, square, frame, red_frame):
    """A portrait slot's sprite: nothing in its first frame, then the picture `square` with the `frame` shape
    over it, concentric, FRAME_SIZE across on the stage; in its third the `red_frame` in its place (Kaniel
    2026-10-18: the member is downed, no badge)."""
    scale = PORTRAIT_SIZE / PORTRAIT_PIXELS
    offset = round(-(FRAME_SIZE - PORTRAIT_SIZE) / 2 / scale * 20)
    ring = matrix(FRAME_SIZE / FRAME_PIXELS / scale, offset, offset)
    frames = [Tag(SHOW_FRAME, b'', False), place(square, 1), place(frame, 2, place_matrix=ring),
              Tag(SHOW_FRAME, b'', False),
              Tag(REMOVE_OBJECT2, struct.pack('<H', 2), False), place(red_frame, 2, place_matrix=ring),
              Tag(SHOW_FRAME, b'', False)]
    sprite = Tag(SPRITE, struct.pack('<HH', char, 3), True)
    sprite.children = frames + [Tag(0, b'', False)]
    return sprite


def portrait_tags(depth, icon_char, strip):
    """The tags that define the portrait slots (their frame from the HUD strip) and place CoopPartyFace0-2 and,
    over them, CoopPartyBadge0-2 (the icon sprite `icon_char`, small), from `depth` up."""
    _, frame_rgba = strip_drawings(strip)
    frame = FIRST_FACE_ID + 3 * len(PARTY_FACES)
    red = frame + 2
    tags, faces, char = [bitmap(frame, frame_rgba, FRAME_PIXELS), bitmap_square(frame + 1, frame, FRAME_PIXELS),
                         bitmap(red, red_ring(frame_rgba), FRAME_PIXELS), bitmap_square(red + 1, red, FRAME_PIXELS)], \
        [], FIRST_FACE_ID
    for slot in range(len(PARTY_FACES)):
        tags += [bitmap(char, bytes(portrait_magic(slot)) * PORTRAIT_PIXELS ** 2, PORTRAIT_PIXELS),
                 bitmap_square(char + 1, char, PORTRAIT_PIXELS), face_sprite(char + 2, char + 1, frame + 1, red + 1)]
        faces.append(char + 2)
        char += 3
    scale = PORTRAIT_SIZE / PORTRAIT_PIXELS
    for index, (name, face) in enumerate(zip(PARTY_FACES, faces)):
        depth += 2
        cx, cy = portrait_centre(index)
        tags.append(place(face, depth, name, matrix(scale, round((cx - PORTRAIT_SIZE / 2) * 20),
                                                    round((cy - PORTRAIT_SIZE / 2) * 20))))
    for index, name in enumerate(PARTY_BADGES):
        depth += 2
        cx, cy = badge_centre(index)
        tags.append(place(icon_char, depth, name, matrix(BADGE_SIZE / 128, round((cx - BADGE_SIZE / 2) * 20),
                                                         round((cy - BADGE_SIZE / 2) * 20))))
    return tags


def shape_bounds(tag):
    """(xmin, xmax, ymin, ymax) in pixels of a DefineShape's bounds."""
    body = tag.body
    nbits = body[2] >> 3
    bits = int.from_bytes(body[2:2 + (5 + 4 * nbits + 7) // 8], 'big')
    total = (5 + 4 * nbits + 7) // 8 * 8
    values = []
    for i in range(4):
        value = (bits >> (total - 5 - nbits * (i + 1))) & ((1 << nbits) - 1)
        values.append((value - (1 << nbits) if value >> (nbits - 1) else value) / 20)
    return tuple(values)


def player_supply_parts(tags):
    """(vial shape, bullet shape, count text) of the player's HUD (PlayerHUD's TriDrug and AmmoItem: the drawing
    each places, and its Stock's Stock text field); None when this HUD has no such parts (a mod's)."""
    defined = {struct.unpack_from('<H', t.body)[0]: t for t in tags
               if t.code in (SPRITE, EDIT_TEXT, 2, 22, 32, 83)}
    first_frame = next(i for i, t in enumerate(tags) if t.code == SHOW_FRAME)

    def child(sprite, name):
        for t in sprite.children:
            if t.code in (PLACE_OBJECT2, PLACE_OBJECT3):
                character, placed, _ = place_parts(t)
                if placed == name and character in defined:
                    return defined[character]
        return None

    def drawing(sprite):
        for t in sprite.children:
            if t.code in (PLACE_OBJECT2, PLACE_OBJECT3):
                character, _, _ = place_parts(t)
                if character in defined and defined[character].code in (2, 22, 32, 83):
                    return character
        return None

    player = next((defined.get(place_parts(t)[0]) for t in tags[:first_frame]
                   if t.code in (PLACE_OBJECT2, PLACE_OBJECT3) and place_parts(t)[1] == 'PlayerHUD'), None)
    if player is None or player.code != SPRITE:
        return None
    vial, ammo = child(player, 'TriDrug'), child(player, 'AmmoItem')
    if vial is None or ammo is None or vial.code != SPRITE or ammo.code != SPRITE:
        return None
    stock = child(vial, 'Stock')
    text = child(stock, 'Stock') if stock is not None and stock.code == SPRITE else None
    vial_shape, ammo_shape = drawing(vial), drawing(ammo)
    if text is None or text.code != EDIT_TEXT or vial_shape is None or ammo_shape is None:
        return None
    return vial_shape, ammo_shape, struct.unpack_from('<H', text.body)[0], defined


def supply_centre(index):
    """The centre of CoopSupply<index>'s vial on the stage: on the name's line, right of most names."""
    return SUPPLY_X, LIST_Y + LIST_STEP * index + BAR_FROM_TAG[1] + SUPPLY_FROM_BAR


def supply_tags(tags, depth):
    """The tags that define CoopSupply0-2 and place them (from `depth` up); none for a HUD without the player's
    vial and bullet."""
    parts = player_supply_parts(tags)
    if parts is None:
        return []
    vial_shape, ammo_shape, text, defined = parts
    out, char = [], FIRST_SUPPLY_ID

    def icon(shape, cx, layer):
        x0, x1, y0, y1 = shape_bounds(defined[shape])
        scale = SUPPLY_ICON / max(y1 - y0, 1.0)
        tx = cx - (x0 + x1) / 2 * scale
        ty = -(y0 + y1) / 2 * scale
        return place(shape, layer, place_matrix=matrix(scale, round(tx * 20), round(ty * 20)))

    def count(name, x, layer):
        # The field's box starts 2 px left of its text; its text 24 px tall: centred on the line.
        y = -(24 * SUPPLY_TEXT_SCALE) / 2 - 3
        return place(text, layer, name, matrix(SUPPLY_TEXT_SCALE, round(x * 20), round(y * 20)))

    def moved(tag, shift):
        """A PlaceObject2 that moves `tag`'s object `shift` px right (its depth, its matrix moved)."""
        _, tx, ty = place_matrix(tag)
        scale = place_matrix(tag)[0]
        depth = place_depth(tag)
        return Tag(PLACE_OBJECT2, bytes([0x05]) + struct.pack('<H', depth) +
                   matrix(scale, tx + round(shift * 20), ty), False)

    for index, name in enumerate(PARTY_SUPPLIES):
        half = SUPPLY_ICON / 2 + 3
        placed = [count(SUPPLY_TEXTS[0], half, 1), count(SUPPLY_TEXTS[1], SUPPLY_STEP + half, 2),
                  icon(vial_shape, 0, 3), icon(ammo_shape, SUPPLY_STEP, 4)]
        frames = placed[:2] + [Tag(SHOW_FRAME, b'', False)] + placed[2:] + [Tag(SHOW_FRAME, b'', False)]
        for step in range(1, SUPPLY_SHIFTS):
            frames += [moved(tag, step * SUPPLY_SHIFT) for tag in placed] + [Tag(SHOW_FRAME, b'', False)]
        sprite = Tag(SPRITE, struct.pack('<HH', char, 1 + SUPPLY_SHIFTS), True)
        sprite.children = frames + [Tag(0, b'', False)]
        out.append(sprite)
        depth += 2
        x, y = supply_centre(index)
        out.append(place(char, depth, name, matrix(None, round(x * 20), round(y * 20))))
        char += 1
    return out


def placed_icon_sprite(tags):
    """The icon sprite CoopPartyIcon0 shows, among these tags."""
    return next(place_parts(tag)[0] for tag in tags
                if tag.code in (PLACE_OBJECT2, PLACE_OBJECT3) and place_parts(tag)[1] == PARTY_ICONS[0])


def place_depth(tag):
    return struct.unpack_from('<H', tag.body, 2 if tag.code == PLACE_OBJECT3 else 1)[0]


def add_party_list(tags, strip=None, portraits=True):
    """Places CoopParty0-2 (the tag sprite RemotePCTag0 shows) and, with the icon strip, CoopPartyIcon0-2 and the
    portraits (CoopPartyFace0-2 and CoopPartyBadge0-2; not with `portraits` false) at the top level, above
    everything, in its first frame; nothing when they are there already."""
    first_frame = next(i for i, tag in enumerate(tags) if tag.code == SHOW_FRAME)
    places = [tag for tag in tags[:first_frame] if tag.code in (PLACE_OBJECT2, PLACE_OBJECT3)]
    names = {place_parts(tag)[1] for tag in places}
    if names >= set(PARTY_LIST):
        # Patched before (an older HUD patch a mod layer still holds): only the icons and portraits it may lack.
        if strip is None:
            return
        depth = max(place_depth(tag) for tag in places)
        added = []
        if not names >= set(PARTY_ICONS):
            added += party_icon_tags(depth, strip)
            depth += 2 * len(PARTY_ICONS)
        if portraits and not names >= set(PARTY_FACES):
            added += portrait_tags(depth, placed_icon_sprite(places + added), strip)
            depth += 2 * (len(PARTY_FACES) + len(PARTY_BADGES))
        if not names >= set(PARTY_SUPPLIES):
            added += supply_tags(tags, depth)
        tags[first_frame:first_frame] = added
        return
    prompt_sprite = next((place_parts(tag)[0] for tag in places if place_parts(tag)[1] == PROMPT_TAG), None)
    tag_sprite = next((place_parts(tag)[0] for tag in places if place_parts(tag)[1] == PARTY_TAG), None)
    if tag_sprite is None:
        return  # a HUD without the remote players' tags: the overlay keeps drawing the list
    depth = max(place_depth(tag) for tag in places)
    added = []
    for name in PARTY_LIST:
        depth += 2
        # PlaceObject2: has a name (0x20), a matrix (0x04, the identity: one zero byte), a character (0x02).
        added.append(Tag(PLACE_OBJECT2, bytes([0x26]) + struct.pack('<HH', depth, tag_sprite) + b'\0' +
                         name.encode('latin-1') + b'\0', False))
    if strip is not None:
        added += party_icon_tags(depth, strip)
        depth += 2 * len(PARTY_ICONS)
        if portraits:
            added += portrait_tags(depth, placed_icon_sprite(added), strip)
            depth += 2 * (len(PARTY_FACES) + len(PARTY_BADGES))
        added += supply_tags(tags, depth)
        depth += 2 * len(PARTY_SUPPLIES)
    if prompt_sprite is not None:
        added.append(place(prompt_sprite, depth + 2, PROMPT,
                           matrix(None, round(PROMPT_AT[0] * 20), round(PROMPT_AT[1] * 20))))
    tags[first_frame:first_frame] = added


def patch_downed_banner(tags):
    """The YouWin banner's two text fields become the yellow YOU ARE DOWN."""
    defined = {struct.unpack_from('<H', tag.body)[0]: tag for tag in tags if tag.code in (SPRITE, EDIT_TEXT)}
    banner = None
    for tag in tags:
        if tag.code == SYMBOL_CLASS:
            count, = struct.unpack_from('<H', tag.body)
            offset = 2
            for _ in range(count):
                character, = struct.unpack_from('<H', tag.body, offset)
                end = tag.body.index(b'\0', offset + 2)
                if tag.body[offset + 2:end] == DOWNED_BANNER.encode():
                    banner = character
                offset = end + 1
    if banner not in defined or defined[banner].code != SPRITE:
        raise ValueError(f'no {DOWNED_BANNER} in this HUD')
    fields = []
    for layer in defined[banner].children:
        if layer.code not in (PLACE_OBJECT2, PLACE_OBJECT3):
            continue
        character, _, _ = place_parts(layer)
        holder = defined.get(character)
        if holder is None or holder.code != SPRITE:
            continue
        for place in holder.children:
            if place.code not in (PLACE_OBJECT2, PLACE_OBJECT3):
                continue
            field, name, _ = place_parts(place)
            if name in (DOWNED_SOURCE_FIELD, DOWNED_FIELD) and field in defined and defined[field].code == EDIT_TEXT:
                fields.append((place, defined[field]))
    if not fields:
        raise ValueError(f'the banner has no {DOWNED_SOURCE_FIELD} text')
    for place, text in fields:
        if place_parts(place)[1] != DOWNED_FIELD:
            rename(place, DOWNED_FIELD)
        parts = edit_text_parts(text)
        if 'text' not in parts or 'color' not in parts:
            raise ValueError("the banner's text field has no text or colour")
        if text.body[slice(*parts['color'])] == bytes.fromhex(DOWNED_COLOR) + b'\xff':
            continue  # done before
        start, end = parts['text']
        html = downed_html(text.body[start:end].decode('latin-1')).encode('latin-1')
        body = text.body[:start] + html + text.body[end:]
        c0, c1 = parts['color']
        text.body = body[:c0] + bytes.fromhex(DOWNED_COLOR) + b'\xff' + body[c1:]


def patch_fe(data, strip=None, portraits=True):
    """The game's HUD movie with the downed banner in place of the YouWin banner and the party list (see the
    module's note); `strip` is the list's icons (party_hud_icons.bin), none without it (and no portraits)."""
    header, tags = read_movie(data)
    patch_downed_banner(tags)
    add_party_list(tags, strip, portraits)
    return write_movie((header, tags))


def hud_source(game, layers):
    """The fe.gfx the game would load with these layers: the last layer that has one, else the game's."""
    import mods
    found = Path(game) / FE
    for _name, root in layers:
        for relative, source in mods.mod_files(root):
            if relative.as_posix().casefold() == FE.casefold():
                found = source
    return found


def with_party_hud(game, layers, out):
    """(layers, notes): the layers with the party HUD over them, made from the HUD they would show (kept in
    out/party-hud/<its hash>, so the same HUD is patched once). A HUD that cannot be patched leaves the layers
    as they are and a note says why: nothing here may stop the game from starting."""
    source = hud_source(game, layers)
    try:
        data = source.read_bytes()
        strip = HUD_ICONS.read_bytes() if HUD_ICONS.is_file() else None
        digest = hashlib.sha256(PATCH_VERSION + data + (strip or b'')).hexdigest()[:16]
        folder = Path(out) / 'party-hud' / digest
        target = folder / FE
        if not target.is_file():
            patched = patch_fe(data, strip)
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_suffix('.part')
            partial.write_bytes(patched)
            partial.replace(target)
    except (OSError, ValueError, IndexError, struct.error) as error:
        return layers, [f'The party HUD is off for this run: {source} ({error})']
    return [*layers, (LAYER, folder)], []
