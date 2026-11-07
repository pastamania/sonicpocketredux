# Camera system

The static camera pass identifies the Plane 2 world-space origin, stage
bounds, and player screen-position targets. These names and call relationships
were cross-checked against ValleyBell's public SPA disassembly release.

## Camera state

| RAM address | Working name | Meaning |
| ---: | --- | --- |
| `0x506C` | `plane2_camera_x` | Plane 2 horizontal world-space origin. |
| `0x506E` | `plane2_camera_y` | Plane 2 vertical world-space origin. |
| `0x507A` | `plane2_camera_x_start` | Horizontal start limit at camera structure offset `+0x0E`. |
| `0x507C` | `plane2_camera_x_end` | Horizontal end limit at camera structure offset `+0x10`. |
| `0x507E` | `plane2_camera_y_end` | Vertical end limit at camera structure offset `+0x12`. |
| `0x5080` | `plane2_camera_y_start` | Vertical start limit at camera structure offset `+0x14`. |
| `0x67A4` | `player_screen_x_current` | Current player horizontal position within the viewport. |
| `0x67A6` | `player_screen_y_current` | Current player vertical position within the viewport. |
| `0x67B8` | `player_screen_x_target` | Optional horizontal target override. |
| `0x67BA` | `player_screen_y_target` | Desired vertical viewport position. |

`initialize_camera_horizontal_follow` at `0x39D0D5` directly establishes:

```text
player_screen_x_current = player_x - plane2_camera_x
```

The render and map-upload paths both read `camera_x` and `camera_y`, while
player/object transforms subtract those values from world coordinates.

## Horizontal following

`update_player_camera_follow` at `0x39C9D9` first uses
`player_screen_x_target` when it is nonzero. Otherwise it selects 48 pixels
when player sprite/facing bit 7 is clear and 112 pixels when bit 7 is set. The
current screen position moves toward that target by no more than two pixels
per player tick.

The routine derives a camera correction from the player world X and the
current follow offset, then clamps the resulting origin to
`camera_min_x..camera_max_x`. Another flag prevents backward camera movement
under normal stage conditions.

This is a look-ahead camera rather than a permanently centered camera: Sonic
can occupy either side of the 160-pixel viewport, and the framing target
slides gradually rather than snapping.

## Vertical following

`apply_vertical_camera_follow` at `0x39C960` compares player Y with
`camera_follow_y_offset`, applies the compact-shape three-pixel adjustment,
and clamps the resulting camera correction to
`camera_min_y..camera_max_y`.

The vertical screen position itself is constrained to 8 through 136 pixels.
`update_player_camera_follow` moves it toward `camera_follow_y_target` by no
more than four pixels per player tick. Static code also includes conditions
that suppress downward or backward correction; these likely cover grounded
camera stability, death, scripted movement, and stage-specific locks.

## Vertical easing in a wide view

Measured on a recording of the first zone (3136 frames): the horizontal camera follows
the player's speed, so its step from one game tick to the next ramps up and down
by itself (3, 3, 4, 3, 4, 5 pixels), but the vertical camera does not move until
the player leaves its window, then moves at once at its full catch-up speed (8 or
9 pixels a tick: `0, 0, 0, 9, 8, 7, 7`) and stops dead (`-10, -11, -10, 0, 0`).
Steps that change by 8 pixels or more between ticks happened 21 times vertically
and 11 horizontally, and a presented picture only fills in between the game's
steps, so the start and stop were as abrupt as on the machine, more visibly at a
higher rate.

A wide view (`Interpolator::ease_at`, `ease_step`) therefore follows the game's
vertical camera with a smoothed one: two cascaded first-order filters (each closing
half of the gap a frame, so the speed is continuous: it picks up and comes to
rest over about five frames), kept within 12 pixels (`WIDE_EASE_MARGIN`) of the
game's. The level is drawn at the smoothed scroll (the level's maps hold the rows
it uncovers, 4 above and 7 below the screen) and every sprite but the HUD and the
title card's moves with it, rounded once so the level and sprites do not shift
against each other. The result on that recording: speed changes of 3 pixels a
frame or more went from 59 to 4, of 4 or more from 28 to 1 (what is left is the
camera jumping, which restarts the filter: a respawn, a warp). The cost is that
Sonic sits up to 12 pixels higher or lower on the screen than the game put him
while the camera catches up (more than 8 pixels on 5% of frames of that recording).

The game drops a hardware sprite that touches none of the screen's 152 lines
(`CopySprites`, `CP DE,0x9E` at `0x23C0E3` and `0x23C189`, the JRs after them at
`0x23C0E6` and `0x23C18C`) and the vertical half of the rectangle test of `0x39D858`
(`CP HL,BC`, the JR at `0x39D882`), so the strips the easing uncovers would be empty
and objects would appear in them as they reached the screen. With a wide view
`wide_sprites.cpp` widens those two tests by 12 pixels, as it does the horizontal
ones (objects' own vertical test, `CP BC,0x118` at `0x23C23D`, is already 64
pixels loose). The picture is eased only while the level is shown from the level
maps with the game's own sprite list, and not for the title card or the act clear,
whose planes are pictures fixed to the screen. The original aspect is as the machine
drew it, and so is `--no-smoothing`.

## Sub-pixel scrolling

While Sonic runs the level moves 7 to 9 pixels a game tick (30 a second), and a presented
picture shows it part of the way through the tick. Placed at whole pixels, the picture of one
refresh moves by a different whole number of pixels from the next: at 144 Hz a steady run is
1, 2, 2, 1, 2 pixels a refresh, at 120 Hz 2, 2, 2, 1 when the tick is 7 pixels and 2, 2, 2, 3 when
it is 9, and 3, 4, 3, 4 at 60 Hz. A slow drift only ever steps 0 or 1, which is why it looked
fine and a run did not. Measured on the recording of the first zone (3136 frames, 8 stretches of
steady running, 790 frames), the spread of the step between one refresh and the next, as a share of
the step, with the glide placed to whole pixels and to a quarter:

| pictures a second | whole pixels | quarter pixel |
| ---: | ---: | ---: |
| 60 | 13% | 10% |
| 90 | 20% | 11% |
| 120 | 19% | 10% |

(At 144: 31% and 12%.) What is left is the game's own steps: Sonic moves whole pixels a tick, so
the camera's step goes 7, 8, 8, 9 and the straight glide changes speed at each tick; the camera
also changes step by 2 pixels or more between ticks 69 times in that recording (1204 ticks).

`Interpolator::render` therefore places the level's scroll planes (`LineRegs::sub1x`, `sub2x`, to
1/`SUBPIXELS` of a pixel, 4) and the sprites (`SpriteShift::sub`) to a fraction of a pixel, in every
aspect. `render_line_subpixels` draws such a line with `SUBPIXELS` samples to a pixel, each plane at
whole pixels a column wider than the view and then laid into the samples that many quarters to the
left, each sprite pixel into `SUBPIXELS` samples that many quarters to the right, and every output
pixel is the average of its samples: a layer a fraction of a pixel across shows its edge pixels
blended, and one on a whole pixel is exactly what `render_line` draws (so a line with nothing
fractional, the picture as the game drew it and `--check-render` are unchanged). A pixel at a
fraction f is the blend of the whole-pixel pictures either side of it, (4-f)/4 of one and f/4 of the
other, which is how it was tested (90 cases of the first zone, 160 and 270 wide, plane 1, plane 2
and sprites: no difference). The extra work is 0.08 ms a picture. `--no-subpixel` (`sonic-pocket`
and `ngpc-run`) places everything at whole pixels. Vertically the easing's smoothed scroll is still
rounded to whole pixels.

## Runtime validation

The BizHawk player tracer records all camera fields. A directed capture remains
useful as a regression fixture and to identify stage-specific locks:

1. Stand still, then run right at full speed for several seconds.
2. Reverse and run left for several seconds.
3. Jump at the bottom of a tall arc or use a vertical spring.
4. Continue until the camera reaches a stage boundary.

The capture will show the exact runtime timing of screen-target changes,
vertical tracking, stage locks, and whether camera values update on the same
30 Hz player cadence.
