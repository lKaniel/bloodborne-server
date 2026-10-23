"""run.py's part of the build check on join: the game's build fingerprint names the patches that change the game
(patches.py's patches-names.json) and hashes patches.bin, so the server can refuse a player whose cheats differ."""
from paths import ROOT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbport_run', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class BuildCheckEnvironmentTests(unittest.TestCase):
    def test_the_patch_names_and_patches_bin_go_to_the_game(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / 'patches-names.json').write_text(
                json.dumps({'gameplay': ['no death', 'no rally'], 'other': ['60 FPS++', 'Disable DoF']}),
                encoding='utf-8')
            env = run.build_check_env(out)
        self.assertEqual(env, {'BB_PATCH_GAMEPLAY': 'no death, no rally',
                               'BB_PATCH_OTHER': '60 FPS++, Disable DoF',
                               'BB_PATCHES_FILE': str(out / 'patches.bin')})

    def test_no_names_file_says_none(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            env = run.build_check_env(out)
            (out / 'patches-names.json').write_text('not json', encoding='utf-8')
            broken = run.build_check_env(out)
        self.assertEqual(env['BB_PATCH_GAMEPLAY'], 'none')
        self.assertEqual(env['BB_PATCH_OTHER'], 'none')
        self.assertEqual(broken['BB_PATCH_GAMEPLAY'], 'none')


if __name__ == '__main__':
    unittest.main()
