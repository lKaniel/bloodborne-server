# Frame-rate fixes

The game was tuned for the PS4's 30 FPS. With a frame rate patch on (60 FPS++, 90 FPS++ or Uncap
FPS++), `scripts/patches.py` (`frame_rate_fixes`) adds these fixes. Without one they are left out:
at 30 FPS they would change nothing.

| Fix | Script | What it does | Off with |
| --- | --- | --- | --- |
| Steady cloth time | `cloth_time.py` | the cloth's time follows the frame's at most 5% a frame, never above 1/30 s, so a stutter no longer kicks capes and dresses | `BB_CLOTH_TIME=0` |
| Steady cloth | `cloth.py` | cloth as at 30 FPS: time-corrected Verlet, damping and bend stiffness per second (needs Steady cloth time) | `BB_STEADY_CLOTH=0` |
| Plant sway time | `foliage_time.py` | plants sway with the frame's (steady) time, not one step a frame | `BB_FOLIAGE_TIME=0` |
| Jumps as at 30 FPS | `air_drag.py` | the air drag per second of 30 FPS, so jumps go as far at any frame rate | `BB_AIR_DRAG=0` |
| Effects in 1/60 s steps | `effect_time.py` | Uncap FPS++ and 90 FPS++ only: effects update in whole 1/60 s steps, so particles no longer pile up (Ebrietas' fight) | `BB_EFFECT_TIME=0` |

`BB_FRAME_RATE_FIXES=0` turns them all off. A fix whose sites hold other code (another game
version, an external patch) is skipped and named in the log. None of them writes within 48 bytes
of a co-op site or of the port's own hooks. The FPS patches' own writes in the effect update are
replaced by Effects in 1/60 s steps, as in the original.

Their code lies in the code segment's last page, past its end (`code_cave.py`). `src/probe.c`
(`patchable`) lets patches write there. Their state is in the data segment's last page, past its
size.

## Credit

From Supermedo's bloodborne_pc windows-v1.9.6: [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)
commits 3b99b54 and c8f68fc (effects), 7a39487, 03094a7 and 0d38fc9 (cloth), e0bf9f4 (plants) and
a83f591 (jumps), GPL-2.0-or-later. Their caves keep the same addresses here. The parts of that page
their port uses for its own features (Quit to Desktop, crash guards, Rebirth, PC Settings) stay
unused here. `probe.c`'s `patchable` comes from the same release. Thanks to Supermedo.
