# SPDX-License-Identifier: GPL-2.0-or-later
"""patches.py's frame-rate fixes (Supermedo's windows-v1.9.6: cloth_time.py, cloth.py,
foliage_time.py, air_drag.py, effect_time.py): only with a frame rate patch, as at 30 FPS nothing
needs them; effects only with Uncap FPS++ or 90 FPS++, as there."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import air_drag  # noqa: E402
import cloth  # noqa: E402
import cloth_time  # noqa: E402
import code_cave  # noqa: E402
import effect_time  # noqa: E402
import foliage_time  # noqa: E402
import patches  # noqa: E402

ORIGINAL = {
    cloth_time.SITE: cloth_time.SITE_BYTES,
    cloth.SITE: cloth.SITE_BYTES, cloth.BEND: cloth.BEND_BYTES, cloth.BATCH: cloth.BATCH_BYTES,
    foliage_time.STORE: foliage_time.STORE_BYTES, foliage_time.KEPT: foliage_time.KEPT_BYTES,
    air_drag.SITE: air_drag.SITE_BYTES,
    effect_time.SITE: effect_time.SITE_BYTES, effect_time.BRANCH: effect_time.BRANCH_BYTES,
}
SEGMENTS = [(0, code_cave.CODE_END), (0x50DC000, 0x56D30F4)]


def image_bytes(start, end, original=ORIGINAL):
    data = original.get(start, b'')
    return (data + bytes(end - start))[:end - start]


def fixes(names, env=None, original=ORIGINAL, segments=SEGMENTS):
    return patches.frame_rate_fixes(names, lambda s, e: image_bytes(s, e, original), segments, env or {})


class FrameRateFixesTest(unittest.TestCase):
    def test_none_at_30_fps(self):
        self.assertEqual(fixes([]), [])
        self.assertEqual(fixes(['Intel tonemap fix']), [])

    def test_uncap_fps_gets_all_five(self):
        named = fixes(['Uncap FPS++', 'Sprint Fix (High FPS)'])
        self.assertEqual(sorted({name for name, _, _ in named}),
                         sorted(['Steady cloth time', 'Steady cloth', 'Plant sway time', 'Jumps as at 30 FPS',
                                 'Effects in 1/60 s steps']))
        # The effects' step: the measured frame time with Uncap FPS++.
        self.assertEqual([w for n, *w in named if n == 'Effects in 1/60 s steps'],
                         [list(w) for w in effect_time.code_writes(measured=True)])

    def test_90_fps_steps_effects_by_its_fixed_step(self):
        named = fixes(['90 FPS++'])
        self.assertIn('Effects in 1/60 s steps', {name for name, _, _ in named})
        self.assertEqual([w for n, *w in named if n == 'Effects in 1/60 s steps'],
                         [list(w) for w in effect_time.code_writes(measured=False)])

    def test_60_fps_has_no_effect_steps(self):
        names = {name for name, _, _ in fixes(['60 FPS++'])}
        self.assertIn('Steady cloth', names)
        self.assertNotIn('Effects in 1/60 s steps', names)

    def test_steady_cloth_needs_the_steady_time(self):
        names = {name for name, _, _ in fixes(['Uncap FPS++'], env={'BB_CLOTH_TIME': '0'})}
        self.assertNotIn('Steady cloth time', names)
        self.assertNotIn('Steady cloth', names)
        self.assertIn('Plant sway time', names)

    def test_each_turns_off(self):
        for env, name in (('BB_STEADY_CLOTH', 'Steady cloth'), ('BB_FOLIAGE_TIME', 'Plant sway time'),
                          ('BB_AIR_DRAG', 'Jumps as at 30 FPS'), ('BB_EFFECT_TIME', 'Effects in 1/60 s steps')):
            self.assertNotIn(name, {n for n, _, _ in fixes(['Uncap FPS++'], env={env: '0'})})
        self.assertEqual(fixes(['Uncap FPS++'], env={'BB_FRAME_RATE_FIXES': '0'}), [])

    def test_other_code_at_a_site_skips_that_fix_only(self):
        changed = dict(ORIGINAL)
        changed[air_drag.SITE] = bytes(len(air_drag.SITE_BYTES))
        names = {name for name, _, _ in fixes(['Uncap FPS++'], original=changed)}
        self.assertNotIn('Jumps as at 30 FPS', names)
        self.assertIn('Steady cloth', names)

    def test_another_segment_layout_skips_them_all(self):
        self.assertEqual(fixes(['Uncap FPS++'], segments=[(0, code_cave.CODE_END - 0x10)]), [])


if __name__ == '__main__':
    unittest.main()
