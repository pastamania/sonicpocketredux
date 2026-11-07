# Controller input

The main loop calls `poll_controller_state` at `0x23C365` once per iteration.
It reads the BIOS-maintained `Controller_Status` byte at RAM `0x6F82` and
updates a six-byte record beginning at `0x4D40`.

## Button bits

The controller byte is active high:

| Bit | Mask | Button |
| ---: | ---: | --- |
| 0 | `0x01` | Up |
| 1 | `0x02` | Down |
| 2 | `0x04` | Left |
| 3 | `0x08` | Right |
| 4 | `0x10` | A |
| 5 | `0x20` | B |
| 6 | `0x40` | Option |

This mapping agrees with the input port definition in MAME's NGPC driver.

## State record

`update_button_state` at `0x23C388` receives a raw button byte and a pointer to
the state record. The fields identified so far are:

| Address | Offset | Working name | Meaning |
| ---: | ---: | --- | --- |
| `0x4D40` | `+0` | `controller_state_current` | Current raw buttons. |
| `0x4D41` | `+1` | `controller_state_pressed` | Newly pressed buttons: `(old ^ current) & current`. |
| `0x4D42` | `+2` | `controller_state_pressed_or_repeat` | New presses plus generated repeat events. |
| `0x4D43` | `+3` | unnamed | Mask involved in held-button repeat generation. |
| `0x4D44` | `+4` | `controller_repeat_counter` | Frames accumulated toward the next repeat event. |
| `0x4D45` | `+5` | `controller_repeat_delay` | Repeat threshold; initialized to six. |

The same helper updates a second record at `0x4D46` from RAM byte `0x6F83`.
The purpose of that secondary source is not yet established, so it remains
unnamed.

## Main-loop use

Immediately after polling, `check_soft_reset_chord` at `0x2001FF` tests whether
`controller_state_current & 0x70` equals `0x70`. Holding A, B, and Option
together therefore disables interrupts, performs shutdown work, restores the
saved stack pointer, and jumps back to `rom_entry`.

The next call, `update_pause_menu` at `0x23BD6F`, consumes new Option presses
from `controller_state_pressed` and dispatches a pause-state jump table. This
is a pause overlay in the main loop rather than the game's top-level mode
dispatcher.

Its state is the byte at `0x49EB`: `0xFF` while the game runs, 0 from the press that opens the
overlay until the one that closes it (the other values the jump table has, 1 to 3, were not seen).
The banner appears two frames after it goes to 0 and is gone a frame after it is `0xFF` again. It is
drawn by scrolling plane 2 by line; see [level-map.md](level-map.md#the-pause-banner).
