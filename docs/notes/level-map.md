# Level maps and widescreen

The levels are plain tables in the cartridge. The hardware keeps only 256x256
pixels of each scroll plane's tile map, and the game fills it just in time, so
the host reads the tables itself to draw a view wider than the 160-pixel screen
(`src/ngpc/level_map.{h,cpp}`).

## Level header

RAM `0x508E` holds a long: the cartridge address of the current level's header.

| Offset | Size | Meaning |
| ---: | --- | --- |
| `+0x00` | word | Width in blocks |
| `+0x02` | word | Height in blocks |
| `+0x0A` | long | Layout of plane 2 (the level): width x height block numbers, row by row |
| `+0x0E` | long | Block table: 32 bytes a block |
| `+0x1A` | long | Layout of plane 1 (a sparse foreground: collectible rings, mostly), same size and block table |

A block is 32x32 pixels: 4x4 tile map entries, row by row, in the K2GE's own
format (tile number, flips and palette). The entry for world tile column `c`
and row `r` is

```text
block = layout[(r / 4) * width + c / 4]
entry = block_table[block * 32 + ((r % 4) * 4 + c % 4) * 2]
```

The other header fields (`+0x04`, `+0x06`, `+0x12`, `+0x16`, `+0x1E` onward)
are not needed for drawing and have not been decoded.

## Camera and the hardware ring

- Tile column `c` of the world is ring column `c & 31`, so the horizontal
  scroll register is the world x modulo 256. The camera in RAM (`0x506C`) tells
  which lap: the world x of the screen is `s2x + 256 * round((camera - s2x) / 256)`.
  The camera and the registers differ by up to about 9 pixels because they are
  updated at different moments.
- The camera's y (`0x506E`) counts up from the bottom of the level: the top of
  the screen is `height * 32 - 152 - camera_y` pixels down from the top.
- Tile row `r` is ring row `(r + k) & 31`, where `k` differs for each level (23,
  31, 27, 3 and 15 for the first five). Rather than derive it, the host finds
  it by trying all 32 and keeping the one under which the decoded tiles equal
  those on screen.
- Both scroll planes use the same scroll registers in a level.
- The camera's own limits are in the structure at `0x506C`: `+0x0E` (`0x507A`)
  is the lowest x it can reach and `+0x10` (`0x507C`) the highest. The first
  level's are 64 and 6176 in a level 6400 pixels wide: the level data has 64
  blank pixels beyond each limit that the game never shows.

## How the game fills the ring

`SomeLvlDraw*` (`0x2918DE` to `0x291C85`) writes a block (4 columns, 4 rows) at
a time, each row of 4 entries in one go. A column of blocks is written when its
left edge comes within about a pixel of the screen's edge, so the ring holds
only 0 to 36 pixels beyond the screen on the side the camera is moving
towards. That is why asking the game to stream further cannot work: a view of
243 pixels needs 9 blocks at some camera positions and the ring holds 8, and
writing a 9th block overwrites columns the view still needs.

## Wide view

For each frame, `LevelMap::build` decodes the tiles of the 20x21 on screen and
compares them with the hardware map. If at least 90% agree (and at least 200
lie inside the level) the screen is a level the decoder describes, and a
`WideScene` is built: the hardware's own entries for the screen itself, the
decoded entries for everything around it. Otherwise the wide view is not used
and the machine's screen is centred with black bars: the title screen, menus,
special stages and anything else that is not drawn from these tables never
match, so no list of such screens is needed.

The view is placed so it never reaches past the camera's range (the lowest
camera x, and the highest plus the screen's 160 pixels): it is centred on the
screen unless the camera is within half the extra width of an end of that
range, in which case the extra width goes to the other side. At the start of a
level Sonic stands near the left edge and the extra width is all on the right.
The scene holds 52 tile columns, 14 of them to the left of the screen's first, which
is enough for the whole extra width to lie on one side (it once held 9 to the
left, and the tiles beyond 72 pixels were missing, as a strip of backdrop at the left
edge, whenever the view had slid left at the end of a level). In the levels of
the table below the camera's range is taken to end at the level's end, not 160
pixels past the camera's last position, since the level goes on for 64 pixels
more than a view's side needs: otherwise the view slid left as the camera
neared the end and snapped back to the middle when it locked. In the other levels
the slide stays. A range narrower than the picture, which is how the game locks the camera in a
boss arena, leaves the rest black, with the HUD at the edges of what is shown
(`--check-wide` counts those frames: a locked range is exactly 160 wide). The
exception is the arenas below.

**The zone title card.** `ShowTitleCard` (`0x3CF75A`) runs as tasks in
`0x3CF75A-0x3CFA96` (the walk of the task list, `0x401A` on, is in
`read_level_ram`, which also keeps the records of those tasks: they own the
card's sprites). The card is a picture for 160 pixels drawn over the level
(plane 1 with the card, the level's plane 2 underneath, faded in from white).
The level is placed as it will be once the card is gone, by the camera's range
as always (at the start of a level: the left edge of the range at the left of
the view, since the level's first 64 pixels are never meant to be seen), and
the card, its plane 1 and the sprites of its tasks (`SpriteShift::card`: the
sprites of those owners, and any sprite that is not a level object's), is drawn
in the middle of the view (`WideScene::centred`, `View::card_origin`). So
nothing moves when the card leaves: it once moved the whole picture 55 pixels
to the left at that moment, and until then showed the level's unseen first
pixels beside the card as bare sky. (The card's silhouette, text and 'ACT'
sprites are therefore 55 pixels to the right of where they stood against the
player in the original, for the few frames the player is visible under it.)

The card's plane 1 is a ring that holds the text, waiting beyond the 160 pixels
of the screen to slide in (`s1x` 128, then 148 to 248 and round to 0). A wider
view sees that waiting text standing at the edges, so while the card has not
moved (`WideScene::card_hold`: plane 1's scroll is still what it was when the card
first showed) plane 1 is drawn only for the card's own 160 pixels, and the text
appears at the edges as it starts to move, as it leaves the same way at the end.

**Act ends and boss arenas.** At the end of an act 1 the camera stops at the
level's end for the capsule (its range collapses to one position, `x_end`); in the
first zone's boss arena the range collapses where the boss was placed
(`Boss0_Load` calls `DoBossCam_X` with the boss's x minus `0x40`); in the second
zone's it is 80 pixels wide. A wide view would be clipped to that range and
left black on the sides. `wide_arena(ram)` says when this is such a place, by the
level number (`0x67D2`: 0, 2, 4, 6, 10 are act 1s, 1 and 3 the first and second
zone's bosses) and a range under 160 pixels, and then the whole level is shown
(`WideScene::arena`, the view centred) and `wide_sprites.cpp` gives the game's
limits on the screen the wider room, below. The level is at least 64 pixels
wider than the camera's last position, so the view never leaves it. The third
zone's boss (level 5) is the end of its level with no lock: nothing is done there
and its pictures are the same as without this.

| Level | Where | What changes in a wide view |
| --- | --- | --- |
| 0, 2, 4, 6, 10 | end of act 1 | no black bars; the player may go to the edges of the view |
| 1 | zone 1 boss (`Boss0_*`, `boss0_main` `0x3A103F`) | as above, and the boss turns round that much further out |
| 3 | zone 2 boss | as above, the boss is as it was |
| 5 | zone 3 boss | nothing |
| 7, 8, 9, 11, 12 | other ends | nothing (not looked at) |

**The game edits its own map.** Rings are tiles of plane 1, and the game wipes
a collected ring by writing `0x0000` over its four tiles in the hardware map;
the level data still has the ring. Each frame, the tiles on screen are compared
with the decoded ones and every difference is remembered by plane, world column
and world row (`LevelMap::m_edits`); where they agree again it is forgotten. The
wide view shows the remembered tile instead of the decoded one beyond the
screen, so a collected ring stays collected when it scrolls into the extra
width. The memory is cleared when the level changes or the screen jumps more
than 128 pixels (a respawn, which has the game draw the map again).

Sprites: a sprite's x is 8 bits, which is ambiguous in a view wider than 256
pixels (16:9 is 270). Sprites that belong to an object in the sprite list are
placed relative to the object's unwrapped x (see `sprites.md`). HUD objects
are moved out to the edges of the picture: those in the left half of the screen
to the left edge, the rest to the right edge. The HUD is recognised by the code
that added its objects, not by its task record: that is at `0x40B2` in some
runs and `0x4196` in others.

## Sprites in the extra width

The game drops sprites that the 160-pixel screen cannot show, in three places, so
without more they would appear only when they reach the edge of the original
screen:

- `AddSpriteObject` (`0x23C224`) refuses an object whose origin is more than 64
  pixels beyond an edge (`x + 64` against `0x120`, compared at `0x23C235`).
- `CopySprites` (`0x23C03D`) drops each hardware sprite that touches no pixel
  of the screen: `DE`, the sprite's x plus 7, must be `0..0xA6` (compared at
  `0x23C0C4` and `0x23C14B`, one for each of its two loops).

- A third test decides whether an object that has a size is updated and drawn
  at all: `0x39D858` (21 callers, among them the moving platforms of the second
  zone, `Obj33` at `0x3DE4CF`) takes a half width in `WA` and a half height in
  `BC`, reads the object's position from its task record (`IZ+0x12` and
  `IZ+0x16`) and the camera from `GetCameraPos`, and returns carry set when the
  rectangle misses the screen. Horizontally it computes `DE = x - camera + half
  width` and `WA = 2 * half width + 160`, then `CP DE,WA` (compared at
  `0x39D872`) and `JR NC` at `0x39D874` skips the object. An object that fails
  does nothing that tick, so a platform appeared as soon as its left edge reached
  the original screen, in the middle of the wide picture.

`wide_sprites.cpp` widens all three when a wide aspect is chosen. It does two
more things in an arena (below). The code is
left alone: a hook (`cpu/tlcs900/instruction_hook.h`, run just before the
instruction at an address in both the interpreter and the recompiled code) at
each of the `JR`s that follow sets the flags to what the comparison would have
produced against the wider range (the third only horizontally: the screen is
no taller in a wide view). With no margin the hooks do nothing. The hook table
lives in `tlcs900.cpp` and no header the generated code includes, so adding it
did not mean regenerating the code.

Over the first zone the sprite table grows from 26.6 to 28.5 sprites on average
(51 at most of 64). The interpreter and the recompiled game produce identical
sprite tables with the hooks on (4500 frames of the second zone).

**Bosses drawn on plane 1.** Both bosses' bodies are a block of plane 1's tiles
(the first zone's 8x4 tiles, the second zone's pod 5x6, in ring columns 0 to
7) that the game moves by scrolling the plane, and the plane is the hardware's
256-pixel ring. Out of sight the block goes into the 96 pixels of the ring a
160-pixel screen never shows; a 270-pixel view shows part of them, so a boss
that was past the left edge also appeared at the right edge, with the half of
it that was out of view. In an arena (when plane 1 does not scroll with the
level and scrolls as a whole) `LevelMap::build` finds the block (the columns
outside the longest empty run), works out which lap of the ring it is in and
sets `WideScene::p1_window`: only 256 pixels of screen x, centred on the block,
are drawn from the ring, so each column shows once, where the block is. The lap
is the one nearest a boss task (`read_level_ram` lists those running code in
`0x3A0EDA-0x3A2000`, with their x less the camera's; the block's middle matched
the boss's x within 4 pixels in both fights), or, if there is none within 64
pixels, the one nearest to where the block was last (the game clears the block
while the boss is well off screen and draws it again at the same edge). Checked
over 9000 frames of the first zone's fight, to both ends of its patrol: the
window's middle was never more than 48 pixels from the boss. The second zone's
pod enters from the right, beyond the edge of the screen, and first appears
there. Lines of text that scroll by themselves (the act clear's) are not
windowed.

**The player and the first boss in an arena.** After the player moves, the game
keeps his screen x between 12 and 148 (`sub_39C47E` at `0x39C47E`: `LDW BC,0x0C`
and `CP IX,BC` at `0x39C496`/`0x39C499`, `LDW BC,0x94` and `CP IX,BC` at
`0x39C49D`/`0x39C4A0`, and when it clamps it zeroes his speed). With the arena
flag set (the host sets it each frame from the last frame's RAM:
`set_wide_arena`), hooks at `0x39C499` and `0x39C4A0` load `12 - left` and
`148 + right` into BC instead, `left` and `right` being the extra width either
side of the screen: the player reaches the edges of a wide view. The first
boss turns round when its x leaves `x_start - 0x48 .. x_end + 0xE8`, which a hook
at `0x3A10EF` widens by the same amounts, so it still goes 72 pixels past the
edges of what is shown. Measured on the first zone's arena (a 16:9 view, 110
extra): the player's screen x goes from 12..148 to -43..203, and the boss's
farthest positions move out by 55 either side; on the second zone's arena the
player's goes the same way. At the end of act 1 it reaches 203.

Unlike the tile maps, this does reach into the game. Objects wake earlier, and a
longer sprite list takes the game longer to build. With the first zone's input
the camera is identical from frame to frame with and without the margin in
levels 0 to 7, 9 and 10, but in levels 8, 11 and 12 (where that input is no
longer a meaningful run) the game's tick lands one frame later in the wide run
from some point, found to be the sprite hooks alone. A recording therefore
plays back exactly only in the aspect it was made in.

A sprite that is not part of a game object (nothing in the object list built
it) is drawn where the stock screen would show it and nowhere else, as its own
x wraps at 256.

## Checking

```text
ngpc-run --rom <cartridge> --input input.txt --frames N --check-wide
ngpc-run ... --level N --check-wide     # play level N (0..12)
ngpc-run ... --aspect 16:9 --png-frames a,b,c
ngpc-run ... --level 1 --warp 4000,5500,600 --input input.txt   # see below
```

`--check-wide` counts the frames where the scene did not hold a tile for every
column the view shows (zero in all 12 levels near their ends, played from
three heights, at both aspects) and any sudden change of how far the view reaches
left of the screen (only respawns, which put the view at the level's left edge). It also compares, for every tile that scrolls onto the screen, what
the game draws with what the wide view showed there one frame earlier: a
difference is a tile that would have popped in at the edge of the original
screen. Over the first zone and the 13 levels that is zero for plane 2 and
for plane 1, apart from a few cells where the game leaves a cell blank (`0x01ff`)
that the level data fills with tile 0 (also blank).

`--level N` makes every read of the level number (RAM `0x67D2`) return `N`, so
each level can be examined without playing to it. `--warp frame,x,y` puts the
player at a position in the level (and the camera 48 pixels left of and 75
below him) at the start of that frame, which is how the far ends of the levels
and the boss arenas were reached; his x and y are those of the task at `0x6708`
(`+0x12` and `+0x16`), the y counted up from the bottom, and he falls from there. `--check-wide` reports how
many frames the decoder reproduced and how well.

On the scripted run through the first zone, and with `--level 0` to `12`, the
decoder agrees with all tiles on screen on every active frame, except for the
partly visible bottom row of tiles on a few frames (the game leaves it blank
and the decoder shows the level's block 0) and the frames of a respawn, where
the game clears the map and redraws it (the picture is blank then, so
switching to the centred view is not visible).

## Not covered

- Special stages have not been looked at.
- Act ends and bosses of the levels not listed above (7 to 9, 11, 12) are as the
  game has them; the other act 1 ends were seen on levels 0, 2, 4 and 6 (level
  10's was not reached).
- Anything the game changes in the map while the tile is off screen would not
  be seen. It has not been observed: the game's edits are made where Sonic is.
- Objects that act before they are drawn: `IsOnScreen` (`0x39F4AC`) and
  `IsInObjectRange` (`0x39F45D`) decide when an object runs (64 pixels beyond
  the edge, and blocks that look to be 192 pixels from the code: not measured).
  Both cover the wide views, and the enemies and monitors of the first zone
  appear in the extra width before the original screen's edge. `AddSpriteObject`
  is given the margin on top of its own 64 pixels, so only an object more than
  about 120 pixels wide beyond its origin could still appear late.
- Only the second zone's moving platform has been watched for the rectangle
  test. With the hook it enters the sprite list when it reaches the bottom edge
  of the screen (at x 193, y 159, still out of sight), where before it entered
  at x 161 and was seen 8 pixels later. The other 20 callers share the test but
  have not been looked at one by one.
