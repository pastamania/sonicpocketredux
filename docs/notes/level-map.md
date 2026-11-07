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

Once it moves, plane 1 is drawn for one lap of the ring only, 256 pixels from
the screen's left edge. The text slides in from beyond the screen's right edge
and goes out the same way, and the ring is 256 pixels round while the view is up
to 270: drawn all round, the ring's last 14 columns, which are the right edge's
too, showed again at the far left, so the end of the sentence flashed there as the
text came in and went again as it moved on.

**Act ends and boss arenas.** At the end of an act 1 the camera stops at the
level's end for the capsule (its range collapses to one position, `x_end`); in the
first zone's boss arena the range collapses where the boss was placed
(`Boss0_Load` calls `DoBossCam_X` with the boss's x minus `0x40`); in the second
zone's it is 80 pixels wide; in the fourth zone's (level 7) it is 16 (6336..6352), in
the fifth's (level 9) 0 to 16 (4288..4304), in the sixth's (level 11) 320 wide while
the boss comes in (3480..3800) and then 0 to 40 (3736..3736, 3716..3756), and the
last fight (level 12) is 480..504, 504..504 and later 424..504. A wide view would
be clipped to that range and left black on the sides (black bars on both sides of
every one of those fights before they were added). `wide_arena(ram)` says when this
is such a place, by the level number (`0x67D2`: 0, 2, 4, 6, 10 are act 1s, 1, 3, 7,
9 and 11 the bosses of the zones and 12 the last fight; `arena_level`) and a range
under 160 pixels, and then the whole level is shown
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
| 7, 9 | zone 4 and 5 bosses (sprites, no plane 1 block) | as for 3 (the player went to both edges of a 16:9 view in each, the bosses went on fighting him) |
| 11 | zone 6 boss (the machine is a plane 1 block, see above) | as for 3, and the machine is drawn where it is (it was gone from the picture all fight) |
| 12 | the last fight | as for 3, the whole hull is drawn in the margins, and black bars from the hit on his pod (see above) |
| 8 | zone 5 act 1 | nothing (the camera is never locked) |
| 13 | Chaotic Space (no level data) | the planes' rings drawn all round (see below); the player may go to the edges of the view; bars from the end of the fight |

**Chaotic Space (level 13).** The last bonus level, Super Sonic against Robotnik's machine in orbit
(the level select's last entry once the game is cleared), has no level behind it: the header pointer
(`0x508E`) is 0, the camera has no range (`0x507A` and `0x507C` are 0) and the decoder reproduced none of
it, so `LevelMap::build` was inactive and a wide view showed the screen centred between black bars. The
picture is two 256-pixel rings that the game scrolls round, the planet (plane 2, with the camera's x) and
the stars (plane 1, by its own scroll), and no seam shows where the ring comes round. `LevelMap::build_space`
makes a scene with `WideScene::ring` set: no tile entries, both planes drawn from the hardware's rings all the
way round (a 270-pixel view sees the first 14 columns of the ring again at its far end), no camera range and
no vertical easing. The level is `LevelRam::space`: the number (`0x67D2`) is 13 *and* a task of the fight's own
code (`0x3D2656` to `0x3D3000`: the controller from `0x3D2682`, the machine's states from `0x3D275B`, the rockets,
`b7_ShowRocket`, `CS_Ring`) or the zone's title card is running, because the number stays 13 on the title
screen and the menus after a play. The card is shown as in any level (`LevelMap::card_state`: centred, held to
its 160 pixels until its text moves). The sprites need nothing but the margins every level has: the machine's
parts, the rockets and Super Sonic are all objects (`--check-centre` finds the middle 160 pixels exactly as
the machine draws them on all 1150 checked frames, at 16:9 and 16:10).

**The machine's body wrapped.** The machine is not made of sprites but of a block of plane 1's tiles (11 columns by
12 rows, 132 tiles, always all in the ring) that its main task moves by the plane's scroll, as the sixth zone's boss and
the last fight's are; only its gun (the object of the task at `0x3D2B12`, 8 or 9 sprites 44 pixels left of the machine's
middle) is sprites, and the flames are in the block. The ring is 256 pixels and a view up to 270, so a block near an
edge showed again at the other: the machine was in the picture twice (in 47 to 59 % of the frames of random play at
16:9). `LevelMap::p1_block_window` (the code the arenas use, now shared) has the picture draw one lap of the
ring, the one the block is in, chosen by the machine's task: its states from the entrance to the end (`CHAOTIC_SPACE_MACHINE`,
`0x3D26B5` on) hold the block's middle in x (`+0x12`), a world x, and the screen's x is that less the camera's
pan (`0x67DC`: 0 to 160, where the level's camera at `0x506C` is a different counter). The machine stays at world x 176,
so on the screen it is at 176 when the pan is 0 and at 16 when it is 160; its entrance runs from -16 across to 208 and back to 176.
The lap is the one that keeps the block within 64 pixels of the task's x (else the one nearest where it was). The
independent check (plane 1 alone, rendered with the scene and the window, pixels more than 100 px from the machine) found
none in 1923 sampled frames for each of four random plays at 16:9 and one at 16:10, and 908 and 1136 of them with the window off.

What does not need changing: with the margin of the view every sprite of the machine that is on the picture is kept
(sprites that the picture cannot show are dropped as in any level: the gun has no sprites on frames where the machine
is above the top of the screen, the plane's block still showing its feet and flames there, as in the original). The
machine's rockets and shots are not removed at the 160-pixel screen's edge (they were seen alive at screen x -122 and 194; the
test they share, `0x3D2B7A`, measures from the end of the camera's range, `0x67E0`, plus `0xC0`), so it is left as it is.

Super Sonic is held to the world by his own routine (`0x39B2EA`, run as he moves): his x (`+0x12` of the
task at `0x6708`) between `0x67DE` + `0x0E` and `0x67E0` + `0x92`, that is 14 pixels in from the screen's
edge at either end of the camera's range (the range is 0 to 160 and the world about 320 wide). The two `CPW`
at `0x39B2F2` and `0x39B302` have the bound in WA, and `space_player_left` and `space_player_right` take the
view's margin off and add it on, as `player_left` does in the arenas (`wide_arena` is true in the level from
the card's end, and `set_wide_arena` carries the margins), so he reaches the edges of the picture: the bounds are x -41 and 361 at
16:9 (-41 to 359 was reached by holding Left, then Right), which leaves the same 14 pixels to the picture's edges.

The fight's end was reached by forcing the machine's task to its descent state (`0x3D2830`; his hit counter is
not understood). The level's byte at `0x6751` then runs 1 (the descent) to 5 and the controller's tasks end
about 700 frames after, with the act clear starting four frames later (`0x6751` = 5, then the player's states
`0x39B11D`, `0x39B14E` ... `0x39B1E0` and 6 to 0xA, Sonic flying over the planet, the plane scene with the
emeralds, and then the ending's tasks). From the controller's end the level is no longer `space`, so the
picture is the centred screen between black bars: the act clear and everything after it are made for 160
pixels, as the last fight's is from the hit on his pod.

**Past the last wall.** The level 7 arena's data stops at x 6528, 16 pixels past the
last position the camera can show (6512), with a brick pillar at the end, and a
view reaching 55 pixels further saw the flat backdrop colour beyond it. The scene
(`LevelMap::scene_entry`) repeats the last tile column of plane 2's last non-blank
block column over the blank ones after it, for the arena levels, so the wall goes
on; the hardware map comparison and the game's edits use the level data as it is.
Levels whose last columns the end fill below has filled have no such columns and
look as before (compared on the act ends and bosses of 0 to 4, 6 and 10).

**The fifth zone's flight (level 8).** The plane Sonic stands on is not made of sprites:
it is a picture in plane 1's ring (the plane is 12 tiles wide in a ring of 32), which the
game slides by changing plane 1's scroll (`s1x` goes 146 to 2 as it comes in from the
left, and 2 to 157 - 256 as it moves to the right of the screen, 101 pixels). The ring is
256 pixels and a 270-pixel view saw it twice: as the plane came in from the left, its
back showed again at the right edge. `plane1_block_level(ram)` (level 8) has
`LevelMap::build` use the one-lap window of a boss's block (`p1_window`), whose lap is
the one the player's task is in (`LevelRam::player_x`, the task at `0x6708`, x at `+0x12`,
less the camera's; the plane's middle was 9 pixels to its left in every frame from the
plane's first appearance to its resting place). Plane 2 and the sprites are as before.
The window is not used while the zone's title card is up (`LevelRam::title_card`): the
card's text is tiles of plane 1 too, and the window, chosen for the plane that has not come
in yet, hid it (the card showed as the silhouette alone). At the flight's end the camera
stops at x 3744 and the level's data at 3904 (the last two blocks are blank), as in any
act, so a centred view reaches 55 pixels past the data: `plane1_block_level` also has
`scene.max_x` go to the level's width (as in the arena levels) and the last tile column
repeated past the data (`scene_entry`, with `extend`), so the view stays centred to the end
and the act clear that follows does not make it jump by 55 pixels. The screen's last, partly
shown tile column (x 3904) belongs to the screen, so the hardware map's blank for it would
have shown as a stripe; past the data it too takes the repeated column.

**The flapping bird robots.** Two of the zone's enemies wait where they are made until their
own test lets them go: after `CALL 0x39D849` (the object's x less the camera's, in IX) the
routine at `0x3E2BD7` does `CP IX,0xB4` / `RET NC` (`0x3E2BE1`, `0x3E2BE5`). The other
enemies of the zone (the propeller robots) wait on the rectangle test, which `wide_sprites.cpp`
widens, so they start where a wide view's edge is. These two did not widen, so they started at
x 177 whatever the view, inside a view that shows 55 pixels more: one at the top (y -15), one
below (y 161), each gliding left along its edge, which a wide view showed as a bird that
appears from nowhere. The hook at `0x3E2BE5` (`bird_wake`) lets them go at `0xB4` plus the
margin: they now start at x 232 (16:9) or 222 (16:10), beyond the picture, and fly in along
the edge. Their path from there is the same: they turn at x 131 (checked: the same x and y
from there as in the original, 54 frames later).

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

## The pause banner

Option in a level opens the game's pause overlay (`update_pause_menu`, `0x23BD6F`; its state byte at
`0x49EB` is `0xFF` while the game runs and 0 while the banner is up). The banner is a picture in plane 2's
ring, three rows of 21 of the 32 columns, in rows just beyond those the screen shows (rows 7 to 9 when
the screen shows rows 18 to 5; 23 to 25 for 3 to 22), which is why the decoder keeps agreeing with the
screen. The game shows it by scrolling plane 2 away from the level on 24 lines in the middle of the screen
(lines 63 to 86: `s2x` 4, `s2y` a place in those rows) and swapping the planes' priority (`pf` `0x80`) on them,
so that plane 2 is over plane 1 there. Nothing else scrolled the planes by line in 9000 frames of each of the 14
levels (scanned with the registers of every line against the first line's); the act clear's text (plane 1, by rows,
`p1_rows`) is the other thing that does, and that scan did not reach it.

A wide view builds plane 2 from the level's tiles with the scroll of the first line, so on those lines it
drew the level's rows at a place they are not (the banner never showed, and the world x taken from a scroll
of 4 put those lines left of the level's data, black at the left); and plane 1's test that it scrolls
with plane 2 on every line failed, which sent the foreground to the hardware's ring. Now:

- a line whose plane 2 scroll is not the first line's is a band line (`LineRegs::band`, set in
  `Interpolator::push`). Its plane 2 is drawn from the hardware's ring, as the title card is, in the middle
  of the view (`View::card_origin`), and the pixels at the screen's left and right edges are carried out to
  the view's edges (`draw_band_plane`), so the banner goes across the picture and its text stays centred;
- it keeps the game's scroll: no glide between ticks and no vertical easing (the banner does not move
  with the level), and the level's world x, so the clipping at the level's ends is the level's;
- plane 1 is tested against plane 2's scroll on the first line (`LevelMap::build`).

The first band line shows one pixel row of the ring row before the banner's, and the last row of the banner
has 7 lines, not 8: the screen the machine draws is the same, and the middle 160 pixels of the wide picture
are exactly that.

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

**Bosses drawn on plane 1.** The bodies of the first, second and sixth zones' bosses are
a block of plane 1's tiles (the first zone's 8x4 tiles, the second zone's pod 5x6, the
sixth's machine 8 columns wide, in ring columns 0 to 7) that the game moves by scrolling
the plane, and the plane is the hardware's
256-pixel ring. Out of sight the block goes into the 96 pixels of the ring a
160-pixel screen never shows; a 270-pixel view shows part of them, so a boss
that was past the left edge also appeared at the right edge, with the half of
it that was out of view. In an arena (when plane 1 does not scroll with the
level and scrolls as a whole) `LevelMap::build` finds the block (the columns
outside the longest empty run), works out which lap of the ring it is in and
sets `WideScene::p1_window`: only 256 pixels of screen x, centred on the block,
are drawn from the ring, so each column shows once, where the block is. The lap
is the one nearest a boss task (`read_level_ram` lists those running code in
`0x3A0EDA-0x3A2000` (the first two) or `0x3A3486-0x3A3F4C` (`Boss5_Load` on: the
sixth), with their x less the camera's; the block's middle matched the boss's x
within 4 pixels in all three fights), or, if there is none within 64
pixels, the one nearest to where the block was last (the game clears the block
while the boss is well off screen and draws it again at the same edge). Checked
over 9000 frames of the first zone's fight, to both ends of its patrol: the
window's middle was never more than 48 pixels from the boss. The second zone's
pod enters from the right, beyond the edge of the screen, and first appears
there.

The sixth zone's boss (level 11; the machine is the block, and the head, the thrusters'
flames and the bombs are sprites) is the same. The plane scrolls from 68 to 208 and
the machine comes in with it from the right, from 220 on the screen (past its 160 pixels)
to 80, and for its first 60 ticks it is wholly beyond the screen, where 220 and -36
are the same place in the ring. The range of boss code was only the first two zones',
so there was no boss task to go by, the lap was taken as the one nearest 80 (the wrong
one: -36), and it was then followed from frame to frame. The machine was drawn 256 pixels
to the left of where it was, off the stock screen and out of the picture, with the head
and the flames alone to be seen, for the whole fight. The range now includes the sixth's
(`Boss5_Load` `0x3A3486` to `Boss6_Load` `0x3A3F4C`). Its controlling task (`0x3A3609`,
`0x3A3648`, `0x3A3680`, `0x3A3694`, `0x3A36DD`) has its x at +0x12, which is the block's
middle to within a pixel from the machine's coming in to the end of what was run (the bombs it
drops are in the range too, and the lap nearest to a boss task is the one with the
controlling task, at no distance, so they do not win). A second task of the machine
(`0x3A088D`) keeps for a few ticks a place on the screen in those words, 124 and 27 and
then 1 and 21, which is not a place in the level (thousands of pixels from it), so a task
whose x less the camera's is 1024 or more from the screen is not taken as a place
(`BOSS_X_LIMIT`). `ngpc-run --check-centre` (the wide picture's own 160 pixels against the
machine's, writing both of the first frame that differs) found it: all of the 160 frames
checked from 3200 differed (the machine was missing), none of the first 800 after the
fix. Over the whole fight (3086 frames, 16:9 and 16:10) 130 frames still differ in
about 200 pixels, 4 to 12 rows of the head, once every 544 frames: the head is a
different frame of its animation in the two pictures (not looked into further; probably
tiles the game rewrites while the picture is being drawn, which a picture taken at the
end of the frame does not have).

**The last fight's hull.** Robotnik's machine in level 12 is a block of plane 1's tiles too
(19 columns of the ring, the hull with its two arms), but its code (`Boss6_Load`
`0x3A3F4C` on; the task of the body is `0x3A088D`, shared with the sixth zone's) draws only the half
of it that is on the screen. The routine that draws it (`0x3A5560`) reads two flag bits that
`0x3A55F0` works out from the boss's x less the camera's, `WA`: the left half is off the screen
for `WA <= 0` (the `JR LE` at `0x3A55FD`), the right half for `WA >= 160` (the `JR LT` at
`0x3A5603` falls through), and the off-screen half's columns are filled with a blank tile
(`FillTileMap`, `0x23C4B1`, called from `0x3A55B3` and `0x3A5593`: columns 0 to 8 or 10 to 18) while
the other half is copied from the boss's picture (`0x23ED1F`, columns 9 to 18 or 0 to 9). A wide
view shows 55 pixels of each half beyond the screen, and the hull had a straight cut down it
whenever the boss's middle was past an edge: the half in the margin was gone, the head, flames
and the arm that flies off (sprites) in their places. `wide_sprites.cpp` has both jumps
decided on the view instead (`HULL_LEFT`, `HULL_RIGHT`: `x <= -margin` and `x >= 160 + margin`; the
flags are the only thing the game uses them for), so the whole hull is drawn while any of it can
be seen. The ring is 256 pixels and the hull 152, so with both halves written the hardware's own
picture shows the hull's far end again at the other edge of the 160 pixels (the ring wraps);
the wide picture draws one lap (`p1_window`, as for the other bosses), so it does not.
`--check-centre` for that reason finds a few edge pixels differ for about 40 frames of the
fight when the boss is far out; they are the wrapped end.

**Robotnik's run in the last level's opening.** At the start of level 12 Robotnik runs across
the screen from the left (a task, `0x40FE` in the lists, whose state is at `0x39EBC8`, the
only level that has one; x from 47 with the camera at 64) and Sonic starts after him. The
state's end is a test of his x less the camera's (`CALL 0x39D849`, `CP IX,0xB0`, `RET LT` at
`0x39EBDE`): from `0xB0` it sets the player's speed (`0x6720` to `0x800`, which is what starts
Sonic) and ends the task. A wide view at the start of a level is all extra width on the right
(the camera is at the range's start, `wide_placement`: 110 pixels in 16:9) and Robotnik
vanished 16 pixels beyond the screen's edge, with 94 pixels of picture still to cross.
`wide_sprites.cpp` (`ACTOR_EXIT`) widens the test by twice the margin, so he runs off the
picture's right edge; Sonic, who goes when he is gone, starts later: Robotnik moves 5 pixels a
tick (30 ticks a second), so 110 more pixels are 22 ticks, about 0.7 seconds in 16:9.

**The last boss's end.** When Robotnik's last hit lands (the boss's hit counter, at +0x39 of the
controller task `0x40FE`, set by `SetBossHits` to 6 or 8 and counted down at `0x3A42BD`, is
zero at the check `0x3A423A`) the controller goes to `0x3A4735` and then through the
states `0x3A4773` (the machine explodes, 90 ticks), `0x3A479B`, `0x3A47AB`, `0x3A482E` and
`0x3A4868`, where it waits while the camera moves on (it is not locked, and ends at 880) and
his pod, a task running `0x3E52EF` on to `0x3E53B4` (spawned by `sub_3E529C` at `0x3A4805`),
comes out of the wreck and hovers in front of the player. That part of the end is shown wide
like the fight: the explosions and the pod's approach are still the level, and the player
has the whole picture to reach it. The pod's task waits in `0x3E53B4` for the player to touch
it (`Coll_TouchPlr`, `0x39D66B`): that second hit is the one that ends the level. It sets the
controller's byte at +0x0C to 10 (the message `0x3A4868` is waiting for), spawns the pieces
that fall (`0x3A0A16`, shown at `0x3A0A6F`) and puts the pod's task in `0x3E540B`, which
flies it off up and to the right. The controller goes on through the act clear (it spawns
`0x3CE7F7`), a short walk across the level with sparkles (`0x3E3586`, `0x3E3616`), and a fade
to white (`0x39E4DF` on), and ends; the ending's task starts a few dozen frames later, with
nothing of its own running between.

All of that is laid out for 160 pixels, like the endings, so `LevelRam::finale` is set from
the frame a task runs `0x3E540B` (`LAST_BOSS_POD_HIT`), in level 12 only, and `ending` with it:
`build` gives no scene and `wide_arena` is false (the player is kept to the screen's own
12..148 again), and the original 160x152 is shown between black bars. The pod's task is
gone before the act clear and the controller's states are not a single run, so no task
of the finale's own is on the list at every frame (the fade before the ending is the longest
gap); `read_level_ram` is therefore given the last frame's `finale` and keeps it until the
level is left (the level number is not 12) or started again (the title card). The bars go up
on the pod's hit and stay up through the act clear, the walk, the fade and the ending's
first picture, which is the next `ending` source below. (Checked by putting the controller's
state pointer at `0x3A4735` during the fight, since the scripted fights never got the boss's
counter to zero, with jumps to touch the pod: wide until the hit at frame 3432, bars from
frame 3433 to the end of the run, 2500 frames later, in every frame; levels 0 to 10 unchanged.)

**Endings and credits.** After the last fight `DoGoodEnding` (or `DoBadEnding`,
`0x3D6EFF` and `0x3D6E6E`) starts the story pictures and `DoCredits` (`0x3E6AD1`): screens
drawn for 160 pixels in the same rings, with the level's header and camera still in RAM and
no level behind them, so a wide view had nothing to show in its margins (and put the
picture and the counters where a level would have them). `read_level_ram` sets
`LevelRam::ending` while a task runs in `0x3D6E6E-0x3D6F79` (up to `DoDelayedBGM`, which
the levels use too) or `0x3E669A-0x3E780E` (the ending scenes and the credits), and `build`
then gives no scene: the screen is shown as every one that is not a level is, the original
160x152 centred between black bars. The bars are already up from the pod's hit (see above); a
run of these tasks alone, as when the ending is started by a hook, goes from the fight to the
bars as the ending's task starts, when it fades the level out. (Checked on both endings, started from the arena
with a hook that spawned the task; the levels' pictures are as before on levels 0 to 3, 5,
7, 8 and 9.)

**The act clear's text.** The "Sonic got through" screen (a task in
`0x3CE60A-0x3CE7F7`, found with the walk of the task list, `LevelRam::act_clear`)
puts each line of text in tile rows of plane 1, each with a scroll value of its
own (the game changes the register between lines), and slides them in by
changing it: "SONIC GOT" from the left, "THROUGH" and the bonuses from the
right. Each is a run of tiles in the 256-pixel ring, and while it waits beyond
the screen it lies in the 96 pixels of ring the screen does not show, where a
view wider than the screen sees it from both sides: the 55 pixels at each edge are
that same stretch, so a line coming in from the right also stood at the far left. Each row therefore gets a window of its own (`WideScene::p1_rows`,
`p1_row_lo`; the same one-lap window as above, in `Interpolator::push` shifted by
the row's scroll): the lap in which the run lies on the screen, or, while it lies
wholly beyond, the lap of the way it last moved (an increasing scroll moves text
left, in from the right), and until it has moved with its text drawn, not drawn
at all, as the original does not show it before it slides in.
(The game sets a row's scroll before drawing the text, so a first jump of the
scroll says nothing about the way.) A row of text with every tile of the ring
filled has nothing to tell apart and is drawn from -48 to 208. Checked on the act
clears of levels 0, 2 and 4, from the first line to the fade-out, at 16:9.
The act clear is a screen laid out for 160 pixels like the title card, with its text,
its sign and the player's pose, and the view is centred on it (`WideScene::act_clear`,
as in an arena) wherever the camera has stopped, with the last tile column repeated past
the data: at the end of the fifth zone's flight, where the camera is not locked, the
placement had put the extra width all on the left and the whole screen stood 55 pixels
to the right of the middle (level 8, found with `--level 8 --warp`).

**The last 64 pixels of a level.** The data of every level runs two blocks (64
pixels) beyond the camera's last position plus the screen (`x_end + 160`), and the
game never shows them, so they were left unfinished: where the last screen ends in
a floor, a row of platforms or a wall, the two columns after it are block 0 (blank),
and the view of an arena, which reaches 55 pixels into them, showed a flat colour
there, and the player, who may walk to the edge of that view, ran off the end of the
floor and died. In the second zone's first act the row of platforms ended exactly at
the screen's edge. `wide_levels.cpp` fills them in: in the arena levels
(`arena_level`: 0 to 4, 6, 7, 9 to 12), the blank blocks of those two columns of plane 2's
layout take the block two columns to their left, row by row, so the same tiles go on
and a row of platforms continues; the game's collision reads the same layout, so
the floor is there (the player was walked along the filled floor of the second zone's
first act to the edge of the view, and on to the end of the act). It is done in the
machine's copy of the cartridge, for a header the first time it is seen, only while
the view is wide, and is put back when the view returns to the original aspect
(`extend_level_ends`; `ngpc-run --no-end-fill` leaves the data alone, to compare).
A column that holds one block all the way down is unfinished too, whatever the block:
the fifth zone's last act (level 9) ends in two columns of sky (block 1) past the end of
the deck and the base's interior, so a 16:9 view of the run to the end of the act showed a
strip of sky where the floor stopped; those two columns now take the two before them, and the
floor and walls go on to the edge of the view. It changes the levels 2 (28 bytes of the
layout), 3 (4), 4 (125), 6 (50), 9 (78) and 10 (43); 0 and 1 had nothing missing. The other levels' ends were not changed: their
cameras stop at the level's end, the view does not reach past it, and the player
stays on the screen.

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
ngpc-run ... --level N --check-wide     # play level N (0..13)
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

`--check-centre` (the middle 160 pixels of a wide picture must be the machine's own screen) over a pause:
`--level N` for 0 to 13, Right held to frame 2399, Option for 10 frames from 2400 (the pause lasts about 100
frames), frames 2380 to 2559 at 16:9. Before the banner was handled 99 of the 180 frames differed in every level
but Chaotic Space (13, which draws the planes from the hardware's ring); now none do, except one frame at
2548 in level 7 (9 pixels on the first line, about 45 frames after the pause ended; it differs the same way without
the change). Everything outside the pause is unchanged: 6300 pictures (every 20th frame of 9000, all 14
levels, 16:9) are identical with and without it.

On the scripted run through the first zone, and with `--level 0` to `12`, the
decoder agrees with all tiles on screen on every active frame, except for the
partly visible bottom row of tiles on a few frames (the game leaves it blank
and the decoder shows the level's block 0) and the frames of a respawn, where
the game clears the map and redraws it (the picture is blank then, so
switching to the centred view is not visible).

## Not covered

- Special stages have not been looked at.
- Chaotic Space was played with a scripted Super Sonic to both edges of the view and to the machine's
  defeat only by forcing it (above); the rest of the fight, with the machine's own attacks, was seen
  only in the pictures. Whether any of its attacks is aimed at a place that the wider bounds change
  was not examined. The scenes after the act clear are centred, not wide, and the picture goes to bars when
  the controller's tasks end, a few frames before the act clear's text appears.
- Level 5 (the third zone's boss) and level 8 are as the game has them (no
  camera lock there; their pictures are the same as without the arena changes).
  The act 1 ends were seen on levels 0, 2, 4 and 6 (level 10's was not reached),
  and the bosses of 7, 9, 11 and 12 only with a scripted player that runs to
  both edges of a 16:9 view: whether each boss's own turn-round limits (other
  than the first boss's) allow the new room was seen, not measured.
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
