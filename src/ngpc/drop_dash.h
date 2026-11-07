// An optional move for Sonic: the drop dash of Sonic Mania.
//
// Jump, then press a jump button again in the air and keep it held: after 20
// frames (10 of the game's ticks) Sonic is charged, a button let go before he
// lands cancels it, and landing with the button still held starts a roll at
// once, with the ground speed
//
//     speed = speed / 4 + base      (speed / 2 when he lands going backwards)
//
// clamped to the cap. The base and the cap are the spindash's weakest and
// strongest launch speeds, read from the cartridge's table, as Mania's drop dash
// (8 to 12) is its spindash's (8 to 12). While he charges he is the solid blue
// ball of the spin, not the frames that show his shoes and eyes. It is not in the
// original game, so it is off until switched on; with it off no hook is installed
// and nothing changes.
//
// It works through hooks (see cpu/tlcs900/instruction_hook.h) at the places in
// the player's code where he jumps, flies, lands and is hurt, with a small state
// per CPU. The charge and the release play the spindash's two sounds with the
// game's own PlaySound; a hook cannot call a routine, so it sends the CPU there
// and gets it back (see HookRedirect). Without a redirect set the dash works
// silently.
//
// A recording made with it on plays back exactly only with it on.
//
// Specific to this cartridge: the addresses belong to its code.
#pragma once

#include "ngpc/hook_call.h"   // set_hook_redirect, redirect_interpreted: how a hook plays a sound

#include <cstdint>

namespace ngpc {

// Switches the drop dash on or off. The hooks are installed the first time it is
// switched on; turning it off drops any charge.
void set_drop_dash(bool enabled);
bool drop_dash_enabled();

} // namespace ngpc
