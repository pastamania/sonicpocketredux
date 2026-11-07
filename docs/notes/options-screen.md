# The Game Option screen

Title menu, Go To Room, Game Option (room `r1B_Options`; the room table entry for `0x1B` is at `0x3D708B`).
One task, `0x3CC51E` when it starts. The host adds rows to it (`src/ngpc/option_screen.cpp`); this note is
what the screen does in the game and how the host works with it.

## The screen

- Plane 1 is the whole picture: a 20 by 19 tile map at `0x9000` (32 words a row; a word is the tile, with the palette
  from bit 9), the checkerboard is plane 2 (`0x9800`), the title is rows 0 and 1 and the highlight bar is 38 sprites.
- A row of the list is one tile row of text at tile rows 3, 5, 7, 9, 11, 13 (and Exit at 17): 16 pixels apart, to
  match the 16-pixel bar. Labels start at tile column 1 (palette 3, light blue) and values end at column 18
  (palette 2, yellow). The labels are baked bitmaps: `DIFFICULTY` is a piece of art cut into tiles, not letters, and
  the tile blob (`0x2791D2`) holds only the lettering these labels and values need.
- Rows, by the number the game's code gives them (task `+0x38`, the cursor): 0 DIFFICULTY, 1 TIME OUT, 2 PLAYER,
  3 AUTO POWER OFF, 4 SOUND TEST, 5 SPECIAL STAGE, 6 EXIT. The task's `+0x3C` is how much is unlocked (0 nothing, 1 the
  Sound Test, 2 that and the Special Stage; from the puzzles done byte `0x6B4A` and the emeralds) and the code skips rows
  by it; SOUND TEST and SPECIAL STAGE are drawn at tile rows 11 and 13 (labels `0x27A264` and `0x27A2B0`) only when
  unlocked.
- Values are maps for the game's drawing code: `[width, height, a word, then the tile words]`, ready to be written to
  plane 1: NORMAL `0x27A224`, EASY `0x27A234`, ON `0x27A240` (2 tiles) and OFF `0x27A248` (3 tiles), and the digits of
  PLAYER by `0x3CC4F4`.
- The bar's task (`0x3CCA3A`, state `0x3CCA4A`) runs every tick: it loads IX with 0x48, IY with the cursor's row from a
  table of words at `0x3CC50C` (31, 47, 63, 79, 95, 111, then 143 for Exit) and does `CALL 0x23C224` (`0x3CCA6E`) to draw
  the layout at `0x2077C6` there.
- Setup is state `0x3CC52A`: it sets `+0x3D` to 0 and the cursor to row 0, loads tiles, palettes and maps, draws the
  values with `ShowVal_Difficulty` (`0x3CC972`), `ShowVal_TimeOut` (`0x3CC9AA`), `ShowVal_AutoPOff` (`0x3CC9E2`) and
  `OptScr_ShowLife` (`0x3CCA1A`), starts the bar, and at `0x3CC62F..0x3CC634` gives the task the state `0x3CC637`
  (one call to `0x23E884`), then the tick state `0x3CC642`. Coming back from the Sound Test or the Special Stage goes through setup again.
- The tick state reads **only** `0x4D41`, the buttons pressed since the last tick (Up 1, Down 2, Left 4, Right 8, A
  `0x10`, B `0x20`), never `0x4D40`. Up and Down move `+0x38` and play sound `0x82`; Left and Right toggle the three
  switches by XOR on `+0x3A` (bit 0 difficulty, 1 time out, 3 auto power off) and write them with `SetDifficultOpt`
  (`0x23B8B3`), `SetTimeOverOpt` (`0x23B8C0`) and `SetAutoPOffOpt` (`0x23B8DA`) or step the lives (`+0x3B`: 1, 3, 5) with
  `SetLifeCntOpt` (`0x23B8CA`), and redraw; A on row 4 plays a sound and sets `+0x3D` to 2 (the Sound Test, started on
  the next tick, `0x3E5AE1`), on row 5 starts the Special Stage (`+0x3D` 1), on row 6 leaves; B (`0x3CC8FE`)
  plays `0x81`, writes the save with `0x23BAC1` (`WriteSRAM`, `0x2375B8`) and leaves through `0x237821`. The `Set*Opt`
  routines only change the RAM mirror of the save (`0x6A60 +` the offset: difficulty `0x6A65`, lives `0x6AE6`, auto power
  off `0x6AE7`, time out `0x6AE8`); the write to flash is on leaving.
- Auto power off's only reader is the main loop (`0x2001BF`), which sets bit 6 of the BIOS's `0x6F86`, a flag this
  runtime never reads: the option does nothing.

## What the host does

With `set_option_screen(true)` (the frontend; `ngpc-run` and `spa-verify` take `--option-screen`, and without it no hook is
installed and nothing changes) the screen reads, top to bottom:

    DIFFICULTY   TIME OUT   PLAYER   DROP DASH   [SOUND TEST   SPECIAL STAGE]   FRAME RATE   ASPECT

SOUND TEST and SPECIAL STAGE stay where the game draws them (tile rows 11 and 13) because the new rows are placed so that
they fall there: DROP DASH is the fourth row (tile row 9, where AUTO POWER OFF was) and FRAME RATE and ASPECT follow,
so with both unlocked all eight rows fill the screen to its last tile row, which is why EXIT goes (B leaves, as before).
With nothing unlocked FRAME RATE and ASPECT are tile rows 11 and 13.

Hooks (`tlcs900_hook`, run before the instruction in interpreted and recompiled code):

| Address | Hook |
| --- | --- |
| `0x3CC62F` end of setup | clears AUTO POWER OFF, the rest and EXIT, draws the host's rows (font tiles into char RAM, labels, values), puts the cursor and bar on the first row |
| `0x3CC642` the tick | the host's cursor and rows (below); then the game is shown the one press it should act on |
| `0x3CCA6E` bar's `CALL 0x23C224` | IY is the row's place: 31 + 16 for each row |
| `0x3CC9EC`, `0x3CC9FA`, `0x3CCA0A` in `ShowVal_AutoPOff` | XBC (where the value of AUTO POWER OFF is drawn, tile row 9) is sent to a place below the picture, as the game draws it again after a change to a row |

The tick hook, for the row the cursor is on (the cursor is the host's, over the list above; `+0x38` is only set when the
game is to act):

- Up and Down move it (wrapping) and play `0x82`;
- B is handed to the game as B: it saves, plays its sound and leaves;
- a row of the game's (DIFFICULTY, TIME OUT, PLAYER): `+0x38` is set to its number and Left or Right (A counts as Right) is handed
  over, so the game toggles it, writes the save's mirror and redraws it, sound included; SOUND TEST and SPECIAL STAGE: `+0x38`
  is set and A is handed over, so the game starts the child;
- a row of the host's: Left, Right or A steps it (the three choices wrap), the row is drawn again and `0x82` is played; the
  frontend reads the change between frames (`take_option_changes`), applies it as its hotkey does and remembers it in
  `settings.txt`, and a hotkey that changes the same thing tells the screen (`set_host_options`);
- the byte `0x4D41` is then set to the press handed over, or 0, so the game's own cursor never moves. While `+0x3D` is not 0
  (the child is being started) the hook does nothing.

The game's save is not touched except by what the game does itself (the three switches, and AUTO POWER OFF's byte as it was:
the game writes back the bit it loaded).

A sound from a hook uses the redirect in `ngpc/hook_call.h` (shared with the drop dash): the hook pushes its own address,
sets WA and sends the CPU to `PlaySound` (`0x3F1883`), which returns to the same instruction; the hook sees the call is over
and puts the registers back. Recompiled code ignores a change to the program counter, so the frontend's redirect unwinds to the
runtime; with none set the rows work silently.

### The font

Text for the host's rows uses a 5 by 7 font of the host's own (the file's `FONT`), drawn bold with a one-pixel shadow in an 8 by 8
tile: colour 2 for the letter and 3 for the shadow, with the palettes of the game's labels (3) and values (2). The 40 tiles go
to char RAM from tile `0x120`; the screen uses tiles up to `0xE4` (the bar's `0xBF..0xE4`). ON and OFF are the game's own.

## Checking it

Headless, from the title: `A` at frame 100 (START and GO TO ROOM), `Down`, `A`, `Up` (the room list wraps to Game Option), `A`.
The pad is polled once a tick, so hold a press at least 4 frames and leave 4 between presses. The screen is up about 100 frames later.

- `ngpc-run --rom <cart> --input nav.txt --option-screen --png-every 12` shows the rows; with the 100% save in the cartridge
  (the last 32 KiB of the ROM) the Sound Test and Special Stage rows are there.
- `spa-verify --rom <cart> --input nav.txt --option-screen --frames 960` keeps the recompiled and the interpreted game in
  exact lockstep through a visit to the screen, a change to every row, the Sound Test, the Special Stage and leaving with B
  (the sound hooks unwind in the recompiled game: the summary shows `unwinds`).
- Without `--option-screen` both are as they were (`ngpc-run --check-render` 0 of 3000, `spa-verify --frames 1200`).

Not covered: a recording made while using the screen replays exactly only with `--option-screen`; the unlock flash tasks
(`0x3CCA97`, `0x3CCACB`, which flash the labels the first time they appear) were not seen with the host's rows, but they
draw only at the rows the game draws SOUND TEST and SPECIAL STAGE at, which the host leaves alone.
