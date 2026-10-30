from paths import ROOT
import os
from pathlib import Path
import struct
import tempfile
import unittest

import hud_patch
import mods

GAME_FE = os.environ.get('BB_GAME_FE')  # the game's menu/fe.gfx, for the check against the real file


def tag(code, body, long=False):
    if len(body) < 0x3F and not long:
        return struct.pack('<H', code << 6 | len(body)) + body
    return struct.pack('<HI', code << 6 | 0x3F, len(body)) + body


def place2(depth, char, name):
    # flags: has name (0x20), has character (0x02), has matrix (0x04: an empty one, a zero byte)
    return tag(26, bytes([0x26]) + struct.pack('<HH', depth, char) + b'\0' + name.encode() + b'\0')


def edit_text(char, html, color=b'\x9b\x21\x02\xff'):
    # rect: nbits 1 (all zero), then flags: HasText | HasColor | HasFontClass + HTML, size 124, var ''
    rect = bytes([0x08, 0x00])
    flags = bytes([0x80 | 0x04, 0x80 | 0x02])
    body = (struct.pack('<H', char) + rect + flags + b'MenuFont_01\0' + struct.pack('<H', 124 * 20) + color +
            b'\0' + html.encode() + b'\0')
    return tag(37, body, long=True)


def shape(char, width, height):
    """A DefineShape `width` x `height` px from the origin, without fills or edges."""
    return tag(2, struct.pack('<H', char) + hud_patch.rect(width * 20, height * 20) + bytes([0, 0, 0, 0]))


def sprite(char, *children):
    return tag(39, struct.pack('<HH', char, 1) + b''.join(children) + tag(1, b'') + tag(0, b''), long=True)


def symbol_class(char, name):
    return tag(76, struct.pack('<HH', 1, char) + name.encode() + b'\0')


def movie(*tags):
    body = bytes([0x08, 0x00]) + struct.pack('<HH', 0x1E00, 1) + b''.join(tags) + tag(0, b'')
    return b'GFX\x0b' + struct.pack('<I', 8 + len(body)) + body


DRAW_HTML = ('<p align="center"><font size="124" color="#9b2102" letterSpacing="0.000000" kerning="1">'
             '<b>HUNTED NIGHTMARE</b></font></p>')
STOCK_HTML = '<p align="left"><font size="24" color="#afafaf"><b>99</b></font></p>'
DEAD_HTML = DRAW_HTML.replace('9b2102', '960807').replace('HUNTED NIGHTMARE', 'YOU DIED')


def fe_movie():
    """A small fe.gfx: TextEffect_Draw (two layers of its text) and TextEffect_Dead, as the game's is built."""
    return movie(
        edit_text(10, DRAW_HTML), sprite(11, place2(1, 10, 'StaticText_522007')),
        edit_text(12, DRAW_HTML), sprite(13, place2(1, 12, 'StaticText_522007')),
        sprite(14, place2(1, 11, ''), place2(3, 13, '')),
        edit_text(20, DEAD_HTML, b'\x96\x08\x07\xff'), sprite(21, place2(1, 20, 'StaticText_522001')),
        sprite(22, place2(1, 21, '')),
        sprite(30, tag(1, b'')), sprite(31, tag(1, b'')),
        # The player's HUD: its vial (TriDrug: a drawing, Stock/Stock) and bullet (AmmoItem: a drawing).
        shape(40, 48, 48), shape(41, 48, 48), edit_text(42, STOCK_HTML),
        sprite(43, place2(1, 42, 'Stock')), sprite(44, place2(1, 40, ''), place2(2, 43, 'Stock')),
        sprite(45, place2(1, 41, '')), sprite(46, place2(1, 44, 'TriDrug'), place2(2, 45, 'AmmoItem')),
        place2(4, 46, 'PlayerHUD'),
        place2(5, 14, 'TextEffect_Draw'), place2(6, 22, 'TextEffect_Dead'), place2(9, 30, 'RemotePCTag0'),
        place2(10, 31, 'ActionButtonTop'),
        tag(1, b''),
        symbol_class(14, 'Fe_fla._TextEffect_Draw_195'))


class GfxTests(unittest.TestCase):
    def test_an_unchanged_movie_is_written_back_byte_for_byte(self):
        data = fe_movie()
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(data)), data)

    def test_the_game_banners_stay_the_games(self):
        # Player report 2026-10-29: killing Mergo's Wet Nurse said YOU ARE DOWN. TextEffect_Draw is the game's
        # NIGHTMARE SLAIN (text 522007, SetTextEffect 17), not an unused PvP banner: no banner is changed.
        data = fe_movie()
        patched = hud_patch.patch_fe(data)
        self.assertEqual(struct.unpack_from('<I', patched, 4)[0], len(patched))
        self.assertNotIn(b'YOU ARE DOWN', patched)
        self.assertNotIn(b'CoopDownedText', patched)
        self.assertEqual(patched.count(b'StaticText_522007'), 2)
        self.assertEqual(patched.count(DRAW_HTML.encode()), 2)
        self.assertIn(b'<b>YOU DIED</b>', patched)
        self.assertIn(b'StaticText_522001', patched)
        _, original = hud_patch.read_movie(data)
        _, tags = hud_patch.read_movie(patched)
        # The banner's sprites and text fields, byte for byte.
        def banner_tags(tags):
            return {struct.unpack_from('<H', t.body)[0]: t.encode() for t in tags
                    if t.code in (hud_patch.SPRITE, hud_patch.EDIT_TEXT) and struct.unpack_from('<H', t.body)[0] <= 14}
        self.assertEqual(banner_tags(tags), banner_tags(original))
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(patched)), patched)

    def test_the_party_list_is_three_more_nameplate_tags_on_top(self):
        _, tags = hud_patch.read_movie(hud_patch.patch_fe(fe_movie()))
        first_frame = next(i for i, t in enumerate(tags) if t.code == hud_patch.SHOW_FRAME)
        places = {hud_patch.place_parts(t)[1]: t for t in tags[:first_frame] if t.code == hud_patch.PLACE_OBJECT2}
        self.assertEqual([places[name].body[3:5] for name in hud_patch.PARTY_LIST], [struct.pack('<H', 30)] * 3)
        self.assertEqual([hud_patch.place_depth(places[name]) for name in hud_patch.PARTY_LIST], [12, 14, 16])
        # The prompt: the game's interact prompt sprite once more, above everything.
        self.assertEqual(hud_patch.place_parts(places['CoopPrompt'])[0], 31)
        self.assertEqual(hud_patch.place_depth(places['CoopPrompt']), 18)

    def test_the_list_icons_are_a_sprite_of_our_drawings_beside_each_tag(self):
        strip = hud_patch.HUD_ICONS.read_bytes()
        patched = hud_patch.patch_fe(fe_movie(), strip)
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(patched)), patched)
        _, tags = hud_patch.read_movie(patched)
        bitmaps = [t for t in tags if t.code == 36]
        # The icons, a placeholder per face, the ring and the downed (red) ring.
        self.assertEqual(len(bitmaps), len(hud_patch.HUD_ICON_FRAMES) - 1 + len(hud_patch.PARTY_FACES) + 2)
        sprites = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == hud_patch.SPRITE}
        first_frame = next(i for i, t in enumerate(tags) if t.code == hud_patch.SHOW_FRAME)
        places = {hud_patch.place_parts(t)[1]: t for t in tags[:first_frame] if t.code == hud_patch.PLACE_OBJECT2}
        icon_char = hud_patch.place_parts(places['CoopPartyIcon0'])[0]
        self.assertEqual(struct.unpack_from('<H', sprites[icon_char].body, 2)[0], len(hud_patch.HUD_ICON_FRAMES))
        self.assertEqual({hud_patch.place_parts(places[name])[0] for name in hud_patch.PARTY_ICONS}, {icon_char})
        # Defined before they are placed, with ids of their own.
        defined = [struct.unpack_from('<H', t.body)[0] for t in tags if t.code in (2, 36, hud_patch.SPRITE)]
        self.assertEqual(len(defined), len(set(defined)))
        self.assertLess(tags.index(sprites[icon_char]), tags.index(places['CoopPartyIcon0']))
        self.assertEqual(hud_patch.patch_fe(patched, strip), patched, 'once')

    def test_each_icon_sits_just_left_of_its_bar_centred_on_it(self):
        # Kaniel 2026-10-13 (rig screenshot): the icon stood 75 px left of the bar and 17 px below it. Measured on
        # a 1080p screenshot of the rig: the tag's HP bar starts at x 109 and runs at y 329.5 for the first tag.
        bar_start = hud_patch.LIST_X + hud_patch.BAR_FROM_TAG[0]
        self.assertEqual(bar_start, 109)
        for index in range(len(hud_patch.PARTY_ICONS)):
            cx, cy = hud_patch.icon_centre(index)
            bar_y = hud_patch.LIST_Y + hud_patch.LIST_STEP * index + hud_patch.BAR_FROM_TAG[1]
            self.assertAlmostEqual(cy, bar_y, delta=1, msg='centred on the bar')
            self.assertEqual(bar_start - (cx + hud_patch.ICON_SIZE / 2), hud_patch.ICON_PAD, 'the padding')

    def test_each_portrait_is_centred_on_its_whole_block_left_of_the_bar(self):
        # Kaniel 2026-10-13: the member's portrait centred vertically on the whole block (the name over
        # the HP bar), left of the bar with the icon's padding; after the 3-player playtest 1.5-2x
        # smaller than the first one (64 px).
        self.assertGreaterEqual(hud_patch.PORTRAIT_SIZE, 64 / 2)
        self.assertLessEqual(hud_patch.PORTRAIT_SIZE, 64 / 1.5)
        # Kaniel 2026-10-17 (the face camera's portraits: "make them a little bit bigger"): 36 to 42.
        self.assertGreaterEqual(hud_patch.PORTRAIT_SIZE, 42)
        bar_start = hud_patch.LIST_X + hud_patch.BAR_FROM_TAG[0]
        for index in range(len(hud_patch.PARTY_FACES)):
            cx, cy = hud_patch.portrait_centre(index)
            bar_y = hud_patch.LIST_Y + hud_patch.LIST_STEP * index + hud_patch.BAR_FROM_TAG[1]
            # The block on a 1080p rig screenshot (2026-10-13): the name's top 34.5 px over the bar's
            # middle, the bar's bottom 4.5 px under it.
            self.assertAlmostEqual(cy, bar_y + (-34.5 + 4.5) / 2, delta=0.5, msg='centred on the block')
            # The padding from the portrait's frame (Kaniel 2026-10-13: a round portrait in a frame).
            self.assertEqual(bar_start - (cx + hud_patch.FRAME_SIZE / 2), hud_patch.ICON_PAD, 'the padding')
            self.assertGreater(cx - hud_patch.FRAME_SIZE / 2, 0, 'on the stage')

    def test_the_portrait_bitmaps_are_sharp_at_4k_and_the_ports_size(self):
        # Kaniel 2026-10-17 ("really poor quality"): the HUD is drawn at the output's size, so a 4K screen
        # shows the portrait twice its stage size; its bitmap is at least that, and the port's texture
        # cache (bbport_portraits.h Side, StageSize) knows the same sizes.
        self.assertGreaterEqual(hud_patch.PORTRAIT_PIXELS, hud_patch.PORTRAIT_SIZE * 2160 / 1080)
        import re
        header = (Path(__file__).resolve().parents[1] / 'gpu' / 'shim' / 'bbport_portraits.h').read_text()
        self.assertEqual(int(re.search(r'constexpr int Side = (\d+);', header).group(1)), hud_patch.PORTRAIT_PIXELS)
        self.assertEqual(int(re.search(r'constexpr int StageSize = (\d+);', header).group(1)),
                         hud_patch.PORTRAIT_SIZE)
        # Party death picks the vial and bullet's frame by these (bloodborne_party.h).
        party = (Path(__file__).resolve().parents[2] / 'emulator/src/core/bloodborne_party.h').read_text(encoding='utf-8')
        self.assertEqual(float(re.search(r'constexpr float SupplyShiftPx = ([0-9.]+)f;', party).group(1)),
                         hud_patch.SUPPLY_SHIFT)
        self.assertEqual(int(re.search(r'constexpr u32 SupplyShifts = (\d+);', party).group(1)),
                         hud_patch.SUPPLY_SHIFTS)

    def test_each_slot_has_the_huds_own_vial_and_bullet_with_their_counts(self):
        # Kaniel 2026-10-18: the vials and bullets in the HUD's own art (the player's TriDrug and AmmoItem): per
        # slot a sprite of two frames (the counts, then the icons too), its count fields named for party death.
        _, tags = hud_patch.read_movie(hud_patch.patch_fe(fe_movie(), hud_patch.HUD_ICONS.read_bytes()))
        first_frame = next(i for i, t in enumerate(tags) if t.code == hud_patch.SHOW_FRAME)
        places = {hud_patch.place_parts(t)[1]: t for t in tags[:first_frame] if t.code == hud_patch.PLACE_OBJECT2}
        sprites = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == hud_patch.SPRITE}
        parts = hud_patch.player_supply_parts(hud_patch.read_movie(fe_movie())[1])
        if parts is None:
            self.skipTest('this HUD has no player vial and bullet')
        vial, bullet, text, _ = parts
        for index, name in enumerate(hud_patch.PARTY_SUPPLIES):
            self.assertIn(name, places)
            sprite = sprites[hud_patch.place_parts(places[name])[0]]
            # The counts, then the icons too at each step right (Kaniel 2026-10-18: they follow the name).
            self.assertEqual(struct.unpack_from('<H', sprite.body, 2)[0], 1 + hud_patch.SUPPLY_SHIFTS)
            inner = [hud_patch.place_parts(t) for t in sprite.children if t.code == hud_patch.PLACE_OBJECT2]
            self.assertEqual([(c, n) for c, n, _ in inner][:4],
                             [(text, 'SupplyVials'), (text, 'SupplyBullets'), (vial, None), (bullet, None)])
            # The last frame moves all four SUPPLY_SHIFT * (SUPPLY_SHIFTS - 1) px right of the first.
            moves = [t for t in sprite.children if t.code == hud_patch.PLACE_OBJECT2 and t.body[0] & 0x01]
            self.assertEqual(len(moves), 4 * (hud_patch.SUPPLY_SHIFTS - 1))
            first_x = hud_patch.place_matrix(sprite.children[0])[1]
            last_x = hud_patch.place_matrix(moves[-4])[1]
            self.assertEqual(last_x - first_x, round(hud_patch.SUPPLY_SHIFT * (hud_patch.SUPPLY_SHIFTS - 1) * 20))
            # On the name's line, right of most names, inside the bar's length.
            _, tx, ty = hud_patch.place_matrix(places[name])
            bar_y = hud_patch.LIST_Y + hud_patch.LIST_STEP * index + hud_patch.BAR_FROM_TAG[1]
            self.assertAlmostEqual(ty / 20, bar_y - 20, delta=1)
            self.assertTrue(180 <= tx / 20 <= 240)

    def test_the_frame_rings_the_portrait(self):
        # Kaniel 2026-10-13: a round portrait in a Bloodborne frame, drawn larger than it shows (crisp when
        # scaled), a few pixels round the picture, shown with it (a second layer of the face sprite's picture
        # frame).
        self.assertGreaterEqual(hud_patch.FRAME_PIXELS, 2 * hud_patch.FRAME_SIZE)
        # The co-op art's ring: its inner gold circle is 69% of its width, just round the picture.
        self.assertAlmostEqual(hud_patch.PORTRAIT_SIZE / hud_patch.FRAME_SIZE, 0.69, delta=0.02)
        strip = hud_patch.HUD_ICONS.read_bytes()
        _, tags = hud_patch.read_movie(hud_patch.patch_fe(fe_movie(), strip))
        first_frame = next(i for i, t in enumerate(tags) if t.code == hud_patch.SHOW_FRAME)
        places = {hud_patch.place_parts(t)[1]: t for t in tags[:first_frame] if t.code == hud_patch.PLACE_OBJECT2}
        sprites = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == hud_patch.SPRITE}
        shapes = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == 2}
        bitmaps = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == 36}
        frame_shapes = set()
        for index, name in enumerate(hud_patch.PARTY_FACES):
            face = places[name]
            scale, tx, ty = hud_patch.place_matrix(face)
            scale = scale or 1
            sprite = sprites[hud_patch.place_parts(face)[0]]
            inner = [t for t in sprite.children if t.code == hud_patch.PLACE_OBJECT2]
            self.assertEqual([hud_patch.place_depth(t) for t in inner], [1, 2, 2],
                             'the picture, the frame over it, then the red frame in its place')
            # The red ring: the same place as the ring, its own drawing, red.
            self.assertEqual(hud_patch.place_matrix(inner[2]), hud_patch.place_matrix(inner[1]))
            red_bitmap = bitmaps[hud_patch.shape_bitmap(shapes[hud_patch.place_parts(inner[2])[0]])]
            red_argb = __import__('zlib').decompress(red_bitmap.body[7:])
            reds = [red_argb[i + 1] > 2 * max(red_argb[i + 2], red_argb[i + 3]) for i in range(0, len(red_argb), 4)
                    if red_argb[i] > 0xC0]
            self.assertTrue(reds and all(reds), 'every drawn pixel red')
            # The frame: concentric with the portrait on the stage, FRAME_SIZE across.
            frame_scale, fx, fy = hud_patch.place_matrix(inner[1])
            size = hud_patch.FRAME_PIXELS * frame_scale * scale
            self.assertAlmostEqual(size, hud_patch.FRAME_SIZE, delta=0.01)
            centre = ((tx + fx * scale) / 20 + size / 2, (ty + fy * scale) / 20 + size / 2)
            for got, want in zip(centre, hud_patch.portrait_centre(index)):
                self.assertAlmostEqual(got, want, delta=0.1)
            frame_shapes.add(hud_patch.place_parts(inner[1])[0])
        self.assertEqual(len(frame_shapes), 1, 'one frame drawing for the three')
        bitmap = bitmaps[hud_patch.shape_bitmap(shapes[frame_shapes.pop()])]
        _, form, width, height = struct.unpack_from('<HBHH', bitmap.body)
        self.assertEqual((form, width, height), (5, hud_patch.FRAME_PIXELS, hud_patch.FRAME_PIXELS))
        import zlib
        argb = zlib.decompress(bitmap.body[7:])

        def alpha(x, y):
            return argb[(y * width + x) * 4]
        half = width // 2
        self.assertEqual(alpha(half, half), 0, 'the portrait shows through')
        self.assertEqual(alpha(2, 2), 0, 'round: its corners clear')
        # The ring just round the picture (64 px of 76 across) is drawn.
        ring = half * hud_patch.PORTRAIT_SIZE / hud_patch.FRAME_SIZE
        self.assertTrue(any(alpha(half, round(half + ring + d)) > 0xC0 for d in range(-1, 4)))
        # Kaniel 2026-10-17: the co-op art's frame and badges (coop_art.py's strip), the frame last.
        import coop_art
        self.assertEqual(coop_art.HUD_COUNT, len(hud_patch.HUD_ICON_FRAMES) + 1)
        self.assertEqual(coop_art.HUD_STRIP, hud_patch.HUD_ICONS)

    def test_the_badge_sits_on_the_frames_top_right(self):
        # Smaller with the portrait, still big enough to read a downed member's revive.
        self.assertTrue(14 <= hud_patch.BADGE_SIZE <= 18)
        for index in range(len(hud_patch.PARTY_BADGES)):
            px, py = hud_patch.portrait_centre(index)
            bx, by = hud_patch.badge_centre(index)
            # Up and right of the portrait's centre, on the diagonal, over the ring a little.
            self.assertGreater(bx, px)
            self.assertLess(by, py)
            self.assertAlmostEqual(bx - px, py - by, delta=0.01)
            distance = ((bx - px) ** 2 + (by - py) ** 2) ** 0.5
            self.assertLess(distance - hud_patch.BADGE_SIZE / 2, hud_patch.FRAME_SIZE / 2, 'over the ring')
            self.assertGreater(distance, hud_patch.FRAME_SIZE / 2 - 2, 'mostly outside it')
            self.assertGreater(distance - hud_patch.BADGE_SIZE / 2, hud_patch.PORTRAIT_SIZE / 2 - 4,
                               'the face uncovered')

    def test_the_portraits_are_placeholder_bitmaps_the_port_fills(self):
        # Each slot's sprite: nothing, then a square of its own bitmap, all one colour (the port's
        # texture cache knows the slot by it and puts the member's picture in its place).
        strip = hud_patch.HUD_ICONS.read_bytes()
        patched = hud_patch.patch_fe(fe_movie(), strip)
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(patched)), patched)
        _, tags = hud_patch.read_movie(patched)
        first_frame = next(i for i, t in enumerate(tags) if t.code == hud_patch.SHOW_FRAME)
        places = {hud_patch.place_parts(t)[1]: t for t in tags[:first_frame] if t.code == hud_patch.PLACE_OBJECT2}
        sprites = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == hud_patch.SPRITE}
        shapes = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == 2}
        bitmaps = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == 36}
        import zlib
        for slot, name in enumerate(hud_patch.PARTY_FACES):
            sprite = sprites[hud_patch.place_parts(places[name])[0]]
            # Kaniel 2026-10-18: nothing, the picture in its ring, the picture in the red ring (downed).
            self.assertEqual(struct.unpack_from('<H', sprite.body, 2)[0], 3, 'nothing, picture, downed')
            shape = shapes[hud_patch.place_parts(next(t for t in sprite.children if t.code == 26))[0]]
            bitmap = bitmaps[hud_patch.shape_bitmap(shape)]
            char, form, width, height = struct.unpack_from('<HBHH', bitmap.body)
            self.assertEqual((form, width, height), (5, hud_patch.PORTRAIT_PIXELS, hud_patch.PORTRAIT_PIXELS))
            argb = zlib.decompress(bitmap.body[7:])
            a, r, g, b = hud_patch.portrait_magic(slot)[3], *hud_patch.portrait_magic(slot)[:3]
            self.assertEqual(argb, bytes((a, r, g, b)) * hud_patch.PORTRAIT_PIXELS ** 2)
            self.assertEqual(r, b, 'the same in RGBA and BGRA')
            self.assertEqual(a, 0xFF)
        self.assertEqual(len({hud_patch.portrait_magic(slot) for slot in range(3)}), 3)
        # The badges: the icon sprite again, small, above the portraits.
        icon_char = hud_patch.place_parts(places['CoopPartyIcon0'])[0]
        for face, badge in zip(hud_patch.PARTY_FACES, hud_patch.PARTY_BADGES):
            self.assertEqual(hud_patch.place_parts(places[badge])[0], icon_char)
            self.assertGreater(hud_patch.place_depth(places[badge]), hud_patch.place_depth(places[face]))
        depths = [hud_patch.place_depth(t) for t in places.values()]
        self.assertEqual(len(depths), len(set(depths)), 'a depth each')
        defined = [struct.unpack_from('<H', t.body)[0] for t in tags if t.code in (2, 36, hud_patch.SPRITE)]
        self.assertEqual(len(defined), len(set(defined)))
        self.assertEqual(hud_patch.patch_fe(patched, strip), patched, 'once')

    def test_a_list_patched_with_icons_before_the_portraits_gets_them(self):
        strip = hud_patch.HUD_ICONS.read_bytes()
        old = hud_patch.patch_fe(fe_movie(), strip, portraits=False)
        self.assertIn(b'CoopPartyIcon0', old)
        self.assertNotIn(b'CoopPartyFace0', old)
        patched = hud_patch.patch_fe(old, strip)
        for name in hud_patch.PARTY_FACES + hud_patch.PARTY_BADGES:
            self.assertEqual(patched.count(name.encode() + b'\0'), 1)
        self.assertEqual(patched.count(b'CoopPartyIcon0'), 1)
        self.assertEqual(hud_patch.patch_fe(patched, strip), patched, 'once')

    def test_the_portraits_make_a_new_patch_version(self):
        self.assertNotIn(hud_patch.PATCH_VERSION, (b'party-hud 5', b'party-hud 4', b'party-hud 3'))

    def test_a_list_patched_before_the_icons_gets_them(self):
        # Kaniel 2026-10-13: the HUD the mods showed was an older patch's (the list, no icons), and the
        # list being there already stopped the icons being added: the party list had no icons.
        strip = hud_patch.HUD_ICONS.read_bytes()
        without = hud_patch.patch_fe(fe_movie())
        self.assertNotIn(b'CoopPartyIcon0', without)
        patched = hud_patch.patch_fe(without, strip)
        self.assertEqual(patched.count(b'CoopPartyIcon'), len(hud_patch.PARTY_ICONS))
        self.assertEqual(patched.count(b'CoopParty0'), 1, 'the list once')
        self.assertEqual(hud_patch.patch_fe(patched, strip), patched, 'once')

    def test_the_matrix_and_rect_bits(self):
        self.assertEqual(hud_patch.matrix(None, 0, 0), bytes([0x02, 0x00]))  # no scale, no rotation, 1-bit moves
        self.assertEqual(hud_patch.rect(0, 0)[0] >> 3, 1)
        bits = hud_patch.BitWriter()
        bits.put(-1, 3, True)
        self.assertEqual(bits.data(), bytes([0b11100000]))

    def test_a_hud_without_remote_tags_gets_no_list(self):
        data = fe_movie().replace(b'RemotePCTag0', b'RemotePCTagX')
        self.assertEqual(hud_patch.patch_fe(data), data)

    def test_the_patch_is_idempotent(self):
        once = hud_patch.patch_fe(fe_movie())
        self.assertEqual(hud_patch.patch_fe(once), once)

    def test_other_files_are_refused(self):
        with self.assertRaises(ValueError):
            hud_patch.patch_fe(b'CWS\x0a' + b'\0' * 20)

    @unittest.skipUnless(GAME_FE and Path(GAME_FE or '.').is_file(), 'BB_GAME_FE: the game\'s menu/fe.gfx')
    def test_the_game_hud(self):
        data = Path(GAME_FE).read_bytes()
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(data)), data)
        patched = hud_patch.patch_fe(data)
        self.assertEqual(patched.count(b'StaticText_522007'), data.count(b'StaticText_522007'))  # NIGHTMARE SLAIN
        self.assertNotIn(b'YOU ARE DOWN', patched)
        for name in hud_patch.PARTY_LIST:
            self.assertEqual(patched.count(name.encode() + b'\0'), 1)
        with_icons = hud_patch.patch_fe(data, hud_patch.HUD_ICONS.read_bytes())
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(with_icons)), with_icons)
        for name in hud_patch.PARTY_ICONS:
            self.assertEqual(with_icons.count(name.encode() + b'\0'), 1)


class LayerTests(unittest.TestCase):
    FE = 'dvdroot_ps4/menu/fe.gfx'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA03173'
        (self.game / 'dvdroot_ps4/menu').mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'exe')
        (self.game / self.FE).write_bytes(fe_movie())
        self.moddir = self.root / 'mods'
        self.moddir.mkdir()
        self.out = self.root / 'out'

    def test_the_layer_patches_the_game_hud_and_goes_over_the_mods(self):
        layers, notes = hud_patch.with_party_hud(self.game, [], self.out)
        self.assertEqual(notes, [])
        self.assertEqual([name for name, _ in layers], [hud_patch.LAYER])
        overlay = mods.build_overlay(self.game, self.out, layers)
        self.assertIn(b'CoopParty0', (overlay / self.FE).read_bytes())
        self.assertEqual((self.game / self.FE).read_bytes(), fe_movie())

    def test_a_mod_hud_is_the_one_patched(self):
        compact = self.moddir / 'Compact HUD' / 'menu'
        compact.mkdir(parents=True)
        (compact / 'fe.gfx').write_bytes(fe_movie().replace(b'YOU DIED', b'YOU DEAD'))
        layers = mods.user_layers(self.game, self.moddir)
        layers, notes = hud_patch.with_party_hud(self.game, layers, self.out)
        self.assertEqual([name for name, _ in layers], ['Compact HUD', hud_patch.LAYER])
        patched = (mods.build_overlay(self.game, self.out, layers) / self.FE).read_bytes()
        self.assertIn(b'YOU DEAD', patched)
        self.assertIn(b'CoopParty0', patched)
        self.assertEqual((compact / 'fe.gfx').read_bytes(), fe_movie().replace(b'YOU DIED', b'YOU DEAD'))

    def test_a_hud_that_cannot_be_patched_leaves_the_layers_and_says_why(self):
        (self.game / self.FE).write_bytes(b'not a movie')
        layers, notes = hud_patch.with_party_hud(self.game, [], self.out)
        self.assertEqual(layers, [])
        self.assertEqual(len(notes), 1)
        self.assertIn('fe.gfx', notes[0])

    def test_the_same_hud_reuses_its_generated_file(self):
        hud_patch.with_party_hud(self.game, [], self.out)
        layers, _ = hud_patch.with_party_hud(self.game, [], self.out)
        generated = list((self.out / 'party-hud').glob('*/' + self.FE))
        self.assertEqual(len(generated), 1)
        self.assertEqual(layers[0][1], generated[0].parents[2])

    def test_a_hud_an_older_patcher_made_is_made_again(self):
        # Kaniel 2026-10-13: the icons fix changed nothing in his game. The file made by the patcher
        # before it was still there under the same name (the HUD and the icons had not changed), and
        # was used as it was.
        import hashlib
        data = (self.game / self.FE).read_bytes()
        strip = hud_patch.HUD_ICONS.read_bytes()
        old = self.out / 'party-hud' / hashlib.sha256(data + strip).hexdigest()[:16] / self.FE
        old.parent.mkdir(parents=True)
        old.write_bytes(hud_patch.patch_fe(data))  # the list without its icons
        layers, _ = hud_patch.with_party_hud(self.game, [], self.out)
        made = layers[0][1] / self.FE
        self.assertNotEqual(made, old)
        self.assertEqual(made.read_bytes().count(b'CoopPartyIcon'), len(hud_patch.PARTY_ICONS))

    def test_the_mods_script_takes_the_party_hud(self):
        import subprocess, sys
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game), '--out',
                                 str(self.out), '--mods-dir', str(self.moddir), '--party-hud'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        overlay = Path(result.stdout.strip())
        self.assertIn(b'CoopParty0', (overlay / self.FE).read_bytes())


if __name__ == '__main__':
    unittest.main()
