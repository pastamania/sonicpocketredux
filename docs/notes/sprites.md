# Sprite objects

The cartridge does not write hardware sprites directly. Objects add entries to
a list, and once per game tick one routine expands the list into the sprite
table. This was found by watching RAM accesses (`Machine::watch_ram`) while
the game ran; `src/ngpc/sprite_trace.cpp` does it permanently so the host knows
which object every sprite belongs to.

## Pipeline

| Stage | Where | What |
| --- | --- | --- |
| Add | `AddSpriteObject` `0x23C224` | A task adds an entry to the list, inserting it in order. Entries off screen (x outside about -0x40..0x120, y beyond 0x118) are not added. |
| Entry pool | RAM `0x4A06`-`0x4B6D` | 30 entries of 12 bytes. The list head is at `0x49FA`, the entry count at `0x4CEF`, and `ClearSprObjects` (`0x23C1F8`) empties it. |
| Expand | `CopySprites` `0x23C03D` | Walks the list and writes one or more hardware sprites per entry into the shadow table at RAM `0x4B6E` (64 sprites of 4 bytes: attribute word, x, y). Sprites it does not use are blanked from `0x23C1C7`. |
| Upload | `CopySprToVDP` `0x23BFE2` (VBlank) | Copies the shadow table to video RAM `0x8800`, and the palette numbers to `0x8C00`. |

`CopySprites` runs once per **game tick, every second frame**. The scroll
registers change on the same frames, so sprites and scenery are in step.

## Entry layout (12 bytes)

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0` | 2 | Next entry in the list. |
| `+2` | 2 | Flags (`0xFF00` seen on every entry). |
| `+4` | 2 | X of the object's origin, sprite coordinates (screen position plus a few pixels). |
| `+6` | 2 | Y, likewise. |
| `+8` | 2 | Low word of the pointer to the sprite layout data (its parts and offsets). |
| `+10` | 2 | Priority. |

An object's sprites are all positioned from its origin, so moving an entry
moves every sprite it produced; the layout changes with each animation frame
and the tile graphics are rewritten into video RAM as well. Slot numbers
therefore say nothing about identity.

## Who owns an entry

At the moment `AddSpriteObject` stores the layout pointer (`0x23C283`),
`XIZ` is the **task record** of the caller (`0x6708` is the player, per
`task-scheduler.md`), and the stack holds the return address, so the call
site is known too. Seen in the first zone:

| Owner (`XIZ`) | Adds |
| --- | --- |
| `0x40B2` or `0x4196` | The HUD (its record is not at a fixed address: `0x40B2` in the scripted run, `0x4196` in another): one entry per element, each from its own call site (`0x39DA51`, `0x39DD05`, ...). |
| `0x6708` | The player: two entries at the same position (`0x39C863`, `0x39C877`), covering up to 14 hardware sprites. |
| other task records | Effects (dust, sparkles) and enemies, one record per object. Some alternate between two call sites (`0x23E582` and `0x23E5A9`) from tick to tick, so the owner alone is the stable part. |

## Which sprites an entry produced

While `CopySprites` expands an entry it reads that entry's bytes. The last entry
read when a shadow sprite is written is the one it came from; writes from
`0x23C1C7` on are blanking. Within one entry some sprites come from a second
emission loop (`0x23C191`, tiles allocated dynamically from video RAM) in which
the entry pointer is not in a register, which is why the read is used.

## Objects that come and go

An object can leave the list for a tick and return. The player does it every
other tick while flashing (hurt, invincible): over 250 transitions in a short
stretch of the first zone, always one tick present, one tick absent. Anything
that blinks does the same. An interpolator that only remembers the last tick's
list sees the returning object as new, so `k2ge_interp.cpp` remembers dropped
objects for 8 ticks and re-matches them by owner and call site.

Effects attached to the player (dust, trail pieces) are separate task records
spawned at his position, so their screen position follows his, not the scenery's.

## How the host uses it

`SpriteTrace` records, for every expansion, the entries (owner, call site,
origin, layout, priority) and the entry of each sprite slot, plus a copy of the
shadow table. `SpriteTrace::match` pairs a captured frame's sprite table with
the expansion that produced it by comparing the 256 bytes. The interpolator
(`k2ge_interp.cpp`) then moves each object as one piece, following it from tick
to tick by owner and call site, and treats sprites that were not built through
the list as fixed on screen.

`AddSpriteObject` also refuses objects whose origin is more than 64 pixels
beyond the screen, and `CopySprites` drops any hardware sprite that touches no
pixel of the 160-pixel screen. A third test, `0x39D858` (21 callers), stops an
object that has a size from updating or drawing at all while its rectangle
misses the screen; the moving platforms of the second zone are among them.
`level-map.md` covers how a wide view relaxes all three.

In a wide view (see `level-map.md`) the object's origin, a signed 16-bit screen
position, also places each of its sprites: a sprite's own x is 8 bits, so it is
put at the position nearest to the object's, which is unambiguous in a view
wider than 256 pixels. The HUD's objects are moved out to the edges of the
picture; they are recognised by the code that added them (`is_hud_caller`), as
the HUD task's record is not at a fixed address.

Checked on the first zone and the title: every frame with visible sprites pairs
with an expansion, and the interpreter and the recompiled game produce
identical tables (entries and slots) over 3000 frames.

## Open questions

- Sprites written some other way than through the list (special stage, text
  screens) have not been looked for.
- Task records are reused, so a new object can briefly share an owner with a
  dead one; the interpolator treats a jump of more than 64 pixels as a new
  object.
- The flags word at `+2` and the high bits of the layout pointer are not
  decoded.
