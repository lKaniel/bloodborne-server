"""Build check on join: the launcher tells the game which co-op release it is (VERSION.txt's version), so the server
can refuse a player on another release with a line that names both versions."""
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))

if sys.platform == 'win32':
    import bbport_launcher_win as launcher


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class ReleaseVersionTests(unittest.TestCase):
    def environment(self, version):
        settings = {**launcher.APP_DEFAULTS, 'game_dir': 'G'}
        with mock.patch.dict(launcher.os.environ), \
                mock.patch.object(launcher, 'load_ini', return_value=({}, [])), \
                mock.patch.object(launcher, 'coop_release_version', return_value=version):
            launcher.os.environ.pop('BB_RELEASE_VERSION', None)
            return launcher.game_environment(settings)

    def test_the_release_goes_to_the_game(self):
        self.assertEqual(self.environment('1.2.0')['BB_RELEASE_VERSION'], '1.2.0')

    def test_a_build_without_a_release_is_dev(self):
        self.assertEqual(self.environment('')['BB_RELEASE_VERSION'], 'dev')


if __name__ == '__main__':
    unittest.main()
