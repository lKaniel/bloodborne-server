"""run.py's party HUD: the port always patches the game's HUD (scripts/hud_patch.py), as its party death is always on.

Party death drives the patched HUD's party list (its icons, badges and prompt); without it they played through
their frames over the HUD (rig 2026-10-13, Kaniel: "all of those bars are flickering and changing"). Party death has
no off switch in the port any more (Kaniel 2026-10-13), so the HUD is patched whatever the old setting says."""
from paths import ROOT
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from test_hud_patch import fe_movie

spec = importlib.util.spec_from_file_location('bbport_run', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class PartyHudTests(unittest.TestCase):
    FE = 'dvdroot_ps4/menu/fe.gfx'

    def test_the_hud_is_always_patched(self):
        self.assertEqual(run.party_hud_args({}), ['--party-hud'])
        # An old kit's PLAY.bat may still set it: party death is on regardless, and so is its HUD.
        self.assertEqual(run.party_hud_args({'SHADPS4_BLOODBORNE_PARTY_DEATH': '0'}), ['--party-hud'])
        self.assertEqual(run.party_hud_args({'SHADPS4_BLOODBORNE_PARTY_DEATH': '1'}), ['--party-hud'])

    def test_bb_party_hud_0_keeps_the_games_hud(self):
        # The HUD patch's own off switch (an A/B of the game's HUD; the overlay draws the list then).
        self.assertEqual(run.party_hud_args({'BB_PARTY_HUD': '0'}), [])

    def test_the_mods_script_gets_the_patched_hud_with_the_old_off_setting(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / 'CUSA03173'
            (game / 'dvdroot_ps4/menu').mkdir(parents=True)
            (game / 'eboot.bin').write_bytes(b'exe')
            (game / self.FE).write_bytes(fe_movie())
            (root / 'mods').mkdir()
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(game), '--out', str(root / 'out'),
                                     '--mods-dir', str(root / 'mods'),
                                     *run.party_hud_args({'SHADPS4_BLOODBORNE_PARTY_DEATH': '0'})],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b'CoopParty0', (Path(result.stdout.strip()) / self.FE).read_bytes())


if __name__ == '__main__':
    unittest.main()
