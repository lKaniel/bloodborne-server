#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Our own machine code in the code segment's last page (game version 01.09).

The code segment ends at CODE_END; up to its 4 KiB page end nothing is there, and the page is
mapped with the segment (probe.c lets patches write there: `patchable`). The frame-rate fixes from
Supermedo's bloodborne_pc windows-v1.9.6 (cloth_time.py, cloth.py, foliage_time.py, air_drag.py,
effect_time.py) keep their caves at the same addresses as there; the parts of the page their port
uses for its own features (Quit to Desktop, crash guards, Rebirth, PC Settings) stay unused here.
`Code` is the small assembler helper of their quit_menu.py.
"""
import struct

EBOOT_BASE = 0x400000
CODE_END = 0x50D96DC         # end of the code segment; up to its 4 KiB page end nothing is there
CAVE = 0x50D9700
CAVE_END = (CODE_END + 0xFFF) & ~0xFFF


class Code:
    """Machine code placed at an image offset, with rip-relative references."""

    def __init__(self, offset):
        self.offset, self.data = offset, bytearray()

    @property
    def here(self):
        return self.offset + len(self.data)

    def raw(self, hexbytes):
        self.data += bytes.fromhex(hexbytes)

    def call(self, target):
        self.data += b'\xe8' + struct.pack('<i', target - (self.here + 5))

    def jmp(self, target):
        self.data += b'\xe9' + struct.pack('<i', target - (self.here + 5))


def check_segments(segments):
    """segments: [(start, end)] of the eboot's PT_LOADs. Raises unless the code segment ends at
    CODE_END and nothing else lies in the rest of its page."""
    if not any(end == CODE_END for _, end in segments) or \
            any(start < CAVE_END and end > CODE_END for start, end in segments if end != CODE_END):
        raise ValueError('code caves: unexpected code segment layout')
