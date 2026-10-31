# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The release notes' what's-new list: without it Discord's post says nothing (v1.3.1).

Run: py -3.12 -m unittest discover -s tools\\release\\tests
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

RELEASE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE))
import release_notes as tool  # noqa: E402

LIST = "**New**\n- A thing.\n\n**Fixed**\n- Another thing.\n"
BODY = ("Intro.\n\n**Already installed?** Press Update.\n\n" + tool.section(LIST, "v9.9.9") +
        "\n\n**New players**:\n\n1. Download the setup.\n\nThe guide.\n\nBuilt from abc.\n")


class Section(unittest.TestCase):
    def test_the_heading_is_written_for_the_list(self):
        self.assertTrue(tool.section(LIST, "v9.9.9").startswith("**What's new in v9.9.9**\n\n**New**"))

    def test_an_empty_list_is_refused(self):
        for text in ("", "  \n", "**New**\n", "Just words.\n"):
            with self.assertRaises(tool.NotesError):
                tool.section(text, "v9.9.9")

    def test_a_list_with_its_own_heading_is_refused(self):
        with self.assertRaises(tool.NotesError):
            tool.section("**What's new since v1**\n- A thing.\n", "v9.9.9")


class Check(unittest.TestCase):
    def test_a_body_with_the_list_passes_with_only_the_list(self):
        posted = tool.check_body(BODY)
        self.assertIn("- A thing.", posted)
        self.assertIn("- Another thing.", posted)
        self.assertNotIn("Download", posted)
        self.assertNotIn("Built from", posted)

    def test_a_body_without_the_list_stops(self):
        with self.assertRaisesRegex(tool.NotesError, "no what's-new"):
            tool.check_body("Intro.\n\n**New players**:\n\n1. Download the setup.\n")

    def test_setup_steps_that_would_reach_discord_stop(self):
        # The bold heading on a line of its own, as v1.3.1's template had it.
        with self.assertRaisesRegex(tool.NotesError, "setup steps"):
            tool.check_body(BODY.replace("**New players**:", "**New players:**"))


class Script(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(RELEASE / "release_notes.py"), *args],
                              capture_output=True, text=True)

    def test_section_prints_the_section_and_body_checks_it(self):
        with tempfile.TemporaryDirectory() as temp:
            notes = pathlib.Path(temp) / "notes.md"
            notes.write_text(LIST, encoding="utf-8")
            result = self.run_tool("section", str(notes), "v9.9.9")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("What's new in v9.9.9", result.stdout)
            body = pathlib.Path(temp) / "body.md"
            body.write_text(BODY, encoding="utf-8")
            self.assertEqual(self.run_tool("check", str(body)).returncode, 0)
            body.write_text("Intro.\n", encoding="utf-8")
            self.assertEqual(self.run_tool("check", str(body)).returncode, 1)

    def test_section_prints_utf8(self):
        with tempfile.TemporaryDirectory() as temp:
            notes = pathlib.Path(temp) / "notes.md"
            notes.write_text("- \u0410 thing.\n", encoding="utf-8")
            result = subprocess.run([sys.executable, "-I", str(RELEASE / "release_notes.py"), "section",
                                     str(notes), "v9.9.9"], capture_output=True)
            self.assertIn("\u0410".encode("utf-8"), result.stdout)

    def test_a_missing_notes_file_stops(self):
        result = self.run_tool("section", "no-such-file.md", "v9.9.9")
        self.assertEqual(result.returncode, 1)


class MakeRelease(unittest.TestCase):
    def setUp(self):
        self.script = (RELEASE.parent / "make-release.ps1").read_text(encoding="utf-8")

    def test_publish_needs_the_notes_file_before_anything_builds(self):
        self.assertIn("[string]$NotesFile", self.script)
        self.assertLess(self.script.index('release_notes.py") section'),
                        self.script.index("# --- 1."))

    def test_the_body_is_checked_before_the_draft(self):
        self.assertLess(self.script.index('release_notes.py") check'),
                        self.script.index("Get-Command gh"))

    def test_the_list_comes_after_already_installed_and_the_heading_ends_it(self):
        at = self.script.index("**Already installed?**")
        self.assertLess(at, self.script.index("$whatsNew", at))
        self.assertIn("**New players**:", self.script)
        self.assertNotIn("**New players:**", self.script)


if __name__ == "__main__":
    unittest.main()
