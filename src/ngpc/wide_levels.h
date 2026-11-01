// Fills in what a level has beyond its last screen, for a wide view.
//
// A level's data runs 64 pixels (two blocks) beyond the last place the camera
// can reach plus the screen's width, and the game never shows them, so the
// designers left them unfinished: where the last screen ends in a floor or a row
// of platforms, those 64 pixels have nothing. A wide view shows up to 55 pixels
// of them, and lets the player walk there (see wide_sprites.h), so he runs off
// the end of the floor into a pit.
//
// Here the blank blocks of the last two columns of a level's tile map take the
// block two columns to their left (the same tiles, so a row of platforms goes
// on). The game reads the same map for what it draws and for what Sonic can
// stand on, so the floor is there to walk on. Nothing is done in the original
// aspect, and when the view returns to it the cartridge is put back as it was.
//
// Specific to this cartridge: the level header (see level_map.h).
#pragma once

#include <cstdint>

namespace ngpc {

class Machine;
struct LevelRam;

// Call every frame with what the game holds about the level (see level_map.h)
// while the view is wide, or with `wide` false otherwise. Only the levels whose
// last screen the wide view can reach (see arena_level) are done, each the first
// time its header is seen.
void extend_level_ends(Machine &machine, const LevelRam &level, bool wide);

} // namespace ngpc
