# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The release's what's-new list, for make-release.ps1 -Publish.

Discord's post (discord_notes.py) is only the notes' "What's new" section, and v1.3.1 went out
with none: its post said nothing. The list comes from -NotesFile, this writes its heading and
checks what Discord will get from the finished notes.

  release_notes.py section <notes file> <tag>   print the section; stops without a list
  release_notes.py check <body file>            stops when Discord would get no list, or the setup steps
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import discord_notes  # noqa: E402

# The setup steps' words: they stay on the GitHub release.
SETUP_WORDS = ("New players", "Built from")


class NotesError(Exception):
    pass


def section(text: str, tag: str) -> str:
    """The notes' what's-new section with `text`, a list of bullets, under its heading."""
    text = text.replace("\r\n", "\n").strip()
    if discord_notes.SECTION.match(text.split("\n")[0].strip()):
        raise NotesError("the notes file has its own what's-new heading: this script writes it")
    if not any(line.lstrip().startswith(("- ", "* ")) for line in text.split("\n")):
        raise NotesError("the notes file has no bullets")
    return f"**What's new in {tag}**\n\n{text}"


def check_body(body: str) -> str:
    """What Discord will post from these notes."""
    posted = discord_notes.changes(body)
    if not any(line.lstrip().startswith(("- ", "* ")) for line in posted.split("\n")):
        raise NotesError("no what's-new list in the notes: Discord would post nothing")
    for word in SETUP_WORDS:
        if word in posted:
            raise NotesError(f'the setup steps would reach Discord ("{word}")')
    return posted


def main(argv: list[str]) -> int:
    try:
        if len(argv) == 3 and argv[0] == "section":
            text = pathlib.Path(argv[1]).read_text(encoding="utf-8")
            sys.stdout.reconfigure(encoding="utf-8")  # -I ignores PYTHONIOENCODING
            print(section(text, argv[2]))
        elif len(argv) == 2 and argv[0] == "check":
            check_body(pathlib.Path(argv[1]).read_text(encoding="utf-8"))
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except (NotesError, OSError) as error:
        print(f"release notes: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
