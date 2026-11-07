// Host options on the game's own Game Option screen.
//
// The screen (title menu > Go To Room > Game Option) lists DIFFICULTY, TIME OUT,
// PLAYER, AUTO POWER OFF, SOUND TEST and SPECIAL STAGE (these two once unlocked), then
// EXIT. With this on, AUTO POWER OFF (which does nothing here) and EXIT (the B button
// leaves, as it always did) are gone, and three rows for the host's own features stand
// where the screen has room for them:
//
//     DIFFICULTY  TIME OUT  PLAYER  DROP DASH  [SOUND TEST  SPECIAL STAGE]  FRAME RATE  ASPECT
//
// DROP DASH is on or off (ngpc/drop_dash.h), FRAME RATE 60, 90 or 120 Hz (the host's picture
// rate) and ASPECT 16:9, 16:10 or ORIGINAL (the host's picture shape). The rows change the
// host's choices, not the game's save: the host reads them back with take_option_changes()
// between frames and applies them, and tells the screen of changes made another way (a hotkey)
// with set_host_options().
//
// How it works: the game builds its screen as ever, and hooks (cpu/tlcs900/instruction_hook.h)
// at the end of its setup draw the rows over it, and at every tick of the screen read the
// buttons first. The cursor is the host's, over a list that includes the rows above; for the
// game's own rows the host hands the game's code the press it should act on (the code reads
// only the "pressed since the last tick" byte, 0x4D41), so the game itself changes difficulty,
// time out and lives, launches the Sound Test and the Special Stage, and saves and leaves on B.
// For every other tick the game is shown no press. The highlight bar's position is set at the
// call that draws it. Text is drawn with a small font of the host's own, in tiles the screen
// leaves free; ON and OFF are the game's own pictures, read from the cartridge.
//
// With the screen off (the default) no hook is installed and nothing changes. It takes the
// same redirect as the drop dash to play the cursor's sound (ngpc/hook_call.h); without one
// the rows work silently.
//
// Specific to this cartridge: the addresses belong to its code.
#pragma once

#include "ngpc/hook_call.h"
#include "ngpc/k2ge_render.h"

namespace ngpc {

struct HostOptions
{
	bool drop_dash = false;
	int refresh = 60;                                                  // 60, 90 or 120
	k2ge_render::Aspect aspect = k2ge_render::Aspect::Wide16x9;
};

// Switches the screen's rows on or off. The hooks are installed the first time it is
// switched on.
void set_option_screen(bool enabled);
bool option_screen_enabled();

// The host's choices, for the screen to show: at start and whenever they change another way.
void set_host_options(const HostOptions &options);

// If the player has changed a row since the last call, the choices as they now stand.
bool take_option_changes(HostOptions &options);

} // namespace ngpc
