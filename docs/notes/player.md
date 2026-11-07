# Player task

The selected-stage flow registers `player_task_init` at `0x399740` in the
static task node at RAM `0x6708`. Its later states read controller input,
choose standing/crouching/looking/moving states, apply Sonic-style ground
physics, perform collision calls, and prepare animation/rendering state.

## Position and velocity

The following fields are confirmed from initialization and the movement
integrators at `0x291606` and `0x291613`:

| Task offset | RAM address | Size | Working name | Meaning |
| ---: | ---: | ---: | --- | --- |
| `+0x10` | `0x6718` | 1 | `surface_angle` | Angle used to resolve scalar ground speed into X/Y velocity. |
| `+0x11` | `0x6719` | 3 | `x_position` | 16.8 fixed-point horizontal position; integer word begins at `+0x12`. |
| `+0x14` | `0x671C` | 1 | `movement_flags` | Includes facing/mode flags; bit meanings remain under analysis. |
| `+0x15` | `0x671D` | 3 | `y_position` | 16.8 fixed-point vertical position; integer word begins at `+0x16`. |
| `+0x18` | `0x6720` | 2 | `ground_speed` | Signed scalar speed along the current surface. |
| `+0x1A` | `0x6722` | 2 | `x_velocity` | Signed 8.8 horizontal velocity. |
| `+0x1C` | `0x6724` | 2 | `y_velocity` | Signed 8.8 vertical velocity. |
| `+0x36` | `0x673E` | 1 | `collision_radius_x` | Horizontal sensor radius; initialized to 7. |
| `+0x37` | `0x673F` | 1 | `collision_radius_y` | Vertical sensor radius; 13 standing and 10 crouching or rolling. |

`velocity_from_angle_and_speed` at `0x291494` indexes two signed lookup tables
with `surface_angle`, multiplies both components by `ground_speed`, and returns
X/Y velocity. The two position integrators then add the low velocity byte to
the fractional position byte and sign-extend the high byte into the integer
word.

## Initial states

`player_enter_idle` at `0x3999F7` clears ground speed and both velocity
components, resets movement flags, selects the standing animation, and changes
the task function to `player_state_idle` at `0x399A2B`.

The standing state demonstrates the input mapping directly:

- Left or Right (`controller_state_current & 0x0C`) enters movement.
- Down enters the crouching state.
- Up enters the look-up state.
- Later crouch/movement states test newly pressed A or B (`0x30`) for actions.

`player_enter_walk` at `0x399EE5` changes the task to `player_state_walk` at
`0x399EF2`. The walking state applies ground acceleration, converts ground
speed through the angle table, performs collision/attachment work, updates
animation, and returns to standing when speed and input conditions permit.

Runtime capture confirms `player_enter_crouch` at `0x399AF3` and
`player_state_crouch` at `0x399B19`. Entering crouch changes the task flags
from `0x01` to `0x05`, changes the vertical radius from 13 to 10, and shifts
integer Y by three pixels. Releasing Down returns to idle and restores the
standing bounds.

## Jump transition

`player_try_jump` at `0x399697` tests newly pressed A or B and requests
`player_enter_jump` through `set_player_state_if_active`. The jump entry at
`0x39AA86`:

1. Plays sound command `0x85`.
2. Selects a jump scalar of `0x0900`, or `0x0490` under a reduced-velocity
   condition that is probably the underwater path.
3. Adds `0x40` to the surface angle to obtain the outward surface normal.
4. Uses `velocity_from_angle_and_speed` and adds the resulting components to
   X/Y velocity.
5. Selects the rolling/jump animation and transitions to airborne movement at
   `0x39AAF7`.

This confirms that the port can share a signed 8.8 velocity representation and
an eight-bit angle system with the original player physics.

The directed standing-jump trace confirms the normal values and sequence:

- The press selects `player_enter_jump` on frame 1774.
- On the next 30 Hz gameplay tick, the compact 7 by 10 bounds and airborne
  flags are active. The jump impulse is `0x0900`; the common airborne update
  immediately applies `0x0080` gravity, leaving logged velocity `0x0880`.
- Gravity subtracts exactly `0x0080` on each later gameplay tick. The recorded
  full-height jump reaches zero vertical velocity at frame 1810.
- While A remains held, the task function stays at
  `player_state_jump_held` (`0x39AAEB`) and executes the shared airborne body.
  Releasing A changes the persistent state to `player_state_airborne`.
- Floor collision selects the walk entry on frame 1846. Two video frames later
  the idle entry restores task flags `0x01`, zero velocity, Y 469, and 7 by 13
  standing bounds.

`limit_jump_velocity_on_release` at `0x39AB8B` implements variable jump height.
When neither A nor B remains held, it caps upward velocity at `0x0400`, or
`0x02E0` under the reduced-velocity condition that is probably underwater.
The first directed trace held A through the apex, so a short-tap capture is
still useful for runtime confirmation of that clamp.

## Spindash and roll

The crouching path enters `player_enter_spindash_charge` at `0x399B98` when a
new A or B press is detected. It plays sound command `0xA3`, selects the
spindash animation, clears the charge word at task offset `+0x3C`, and changes
the task function to `player_state_spindash_charge` at `0x399BB7`.

While Down remains held, `update_spindash_charge` at `0x399B5E` decays the
existing value by one thirty-second each frame. Each new A or B press adds
`0x0200`, caps the value at `0x0800`, and retriggers sound `0xA3`. Releasing
Down plays sound `0xA2`, indexes the launch-speed table at `0x3993BC`, writes
the result to `ground_speed`, and enters rolling.

`player_enter_roll` at `0x399F3B` selects the rolling collision shape before
entering `player_state_roll` at `0x399F48`. The shape transition is explicit:

- Standing uses radii 7 by 13.
- Rolling uses radii 7 by 10 and shifts the integer Y position by three pixels
  to keep the contact point stable.
- Task flag `+0x0B` bit 2 records the compact crouching/rolling collision mode.

The rolling state applies lower-friction ground physics, resolves surface
collision, permits jumping, and returns to idle when speed drops below the
exit threshold.

## Runtime-confirmed flags and cadence

The first BizHawk 2.10 player trace contains 1,027 video frames. Every one of
the 513 even-to-odd frame pairs has identical player state. Movement advances
only on the next even frame, establishing a 30 Hz player gameplay tick over
the roughly 60 Hz video output for this scene.

The same trace confirms two task flag bits at offset `+0x0B`:

- Bit 1 (`0x02`) is the airborne flag. It appears when
  `player_enter_airborne` at `0x39AAC6` advances to the common airborne state.
- Bit 2 (`0x04`) selects the compact 7 by 10 collision bounds used while
  crouching, charging a spindash, rolling, and during a rolling airborne arc.

`surface_angle` uses a full 256-unit turn. Zero is level rightward movement;
angle `0x0A` produced positive X and Y velocity on an ascending slope, while
`0x40` produced vertical movement at the end of the captured rolling path.
Once airborne, `relax_airborne_surface_angle` at `0x39ABCA` moved the angle
`0x40`, `0x3C`, `0x38`, and so on toward zero by four units per gameplay tick.

These observations come from BizHawk runtime traces captured during the first
attempt at this project.

## Hurt, death, and spring launch

`player_enter_hurt` at `0x39AA54` applies horizontal knockback of `0x0200`
away from the damaging object and a normal upward impulse of `0x0600`. It sets
task offset `+0x48` to `0x3C`, enables airborne movement, and uses the common
airborne state for gravity and collision. The first post-hit trace sample is
therefore `0x0580` after one `0x0080` gravity step.

The death path consists of three states:

- `player_enter_death` at `0x39A9BF` clears horizontal velocity, sets movement
  flags `0x18`, and launches upward at `0x0800`.
- `player_state_death_motion` at `0x39A9EA` subtracts `0x0080` gravity and
  integrates Y until the player crosses the camera boundary.
- `player_state_death_delay` at `0x39AA11` holds the final position until the
  task is removed and the stage restart begins.

The captured vertical spring supplied Y velocity `0x0D00` before selecting
`player_enter_spring_launch` at `0x39A98A`. That entry chooses the spring
animation, applies airborne flags, clears compact collision mode, and returns
to `player_state_airborne`. The next tick records `0x0C80` after gravity.

## Landing, and the optional drop dash

The player's code keeps the task pointer in `XIZ`, and sets the task's state by
loading a routine's address into `XWA` and storing it at `(XIZ)`. The airborne
state's body (`0x39AAF7`, which the jump's first ticks reach from `0x39AAEB` while a
jump button is held) ends with the landing: after `Plr_IsOnGround` reports the
ground it calls `0x39ABEC` and `0x3993CE`, which turn the air velocity into the
ground speed at `+0x18` for the surface under him, and then

    39AB49  LD XWA,0x399EE5        Plr_Walking
    39AB4E  LD (XIZ),XWA           the new state
    39AB50  BIT 1,(0x4D40)         Down held?
    39AB54  JR Z,0x39AB64
    39AB56  CP (XIZ+0x1A),0        moving?
    39AB5B  JR Z,0x39AB64
    39AB5D  LD XWA,0x399F3B        Plr_Rolling
    39AB62  LD (XIZ),XWA
    39AB64  CALL 0x39ABCA          Plr_RotateTo0, then SetupObjAni

so landing into a roll is already a path of the game. Observed with `ngpc-run`:

- `+0x18` is the ground speed in the direction he faces, signed, so it is
  negative when he lands going backwards (jumping right, turning left in the air
  makes him face left at once, `+0x14` bit 7 set, with `+0x1A` still positive).
  Walking tops out at about `0x0860`; the spindash's launch speeds (table at
  `0x3993BC`, nine words, charge 0 to 8) run `0x0A00` to `0x0E00`. Rolling on the
  level loses `0x10` a tick.
- `+0x14` bit 7 is set when he faces left. Down is bit 1 (`0x02`) of the buttons
  at `0x4D40`; A is `0x10` and B `0x20`.
- The airborne body runs once per game tick (30 Hz).

`--drop-dash` (see `src/ngpc/drop_dash.h`) adds the drop dash of Sonic Mania with
hooks at these places. It is not in the game and is off by default:

| Address | Hook |
|---|---|
| `0x39AA86` Plr_Jumping | arms it (a jump, not a fall, spring or hit) |
| `0x39AAF7` airborne body | a new press of A or B after the jump starts the charge; held for 10 ticks (Mania's 20 frames) it is charged, and `PlaySound(0xA3)` (the spindash's charge) plays; letting go cancels it. While it charges, `+0x1E` (see below) is put at `0x398B26` every tick so that he stays the solid ball |
| `0x39AB4E` the landing's store | charged and still held: `PlaySound(0xA2)` (the spindash's release), `+0x18` becomes `speed/4 + 0x0A00` (`speed/2` when negative), at most `0x0E00`, and `XWA` is made Plr_Rolling, so the store makes the roll |
| `0x39AA54` Plr_Hurt, `0x39A98A` spring launch, `0x39A9BF` death | drops a charge |

The jump's spin is a script at `0x398B20`: eight entries of six bytes (`0x000C`,
frame word, `0x0103`) with the frames 12 16 14 15 12 13 16 15 and then a jump back
(`02 20 8B 39`). The task keeps the address of the entry to run next at `+0x1E` (a
long), advancing six bytes a tick, and the picture shows an entry a tick after it
ran. Frame `0x16` (the entries at `0x398B26` and `0x398B44`) is the solid blue ball
(a 3 by 3 block of sprites); the others show his shoes and eyes (twelve sprites).
Keeping `+0x1E` at `0x398B26` shows the ball every tick, and `+0x15` bit 7, which
flips between ticks, makes no difference to it.

The sounds are played by sending the CPU to `PlaySound (0x3F1883)` with a
return address of the hooked instruction, which runs its hook again: a hook can
not call a routine, and recompiled code ignores a changed program counter, so
the hosts give the hooks a redirect (`rt::unwind` for recompiled code, the
program counter for the interpreter). `spa-verify --input buttons.txt
--drop-dash` runs both machines through a dash in exact lockstep.

## Collision overview

Grounded movement converts `ground_speed` and `surface_angle` into X/Y
velocity, advances the position, then calls the surface collision routines.
Airborne movement follows a separate velocity-directed resolver. The detailed
sensor pipeline and scratch layout are recorded in [collision.md](collision.md).

## Still to map

The next player pass should finish the remaining movement flag bits and trace
camera and stage-object state. A short-tap jump trace can confirm the
variable-height velocity clamp. Movement flag `+0x14` bit 7 controls facing and
horizontal mirroring (set when he faces left, see the landing section).
