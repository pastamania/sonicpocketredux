// Lets the game keep sprites for the extra width of a wide view.
//
// The game builds its sprite table from a list of sprite objects, and drops
// everything the 160-pixel screen cannot show: whole objects whose origin is
// more than 64 pixels beyond an edge (AddSpriteObject), any hardware sprite
// that touches no pixel of the screen (CopySprites), and, before an object with
// a size (a moving platform, say) is updated or drawn at all, one that misses
// the screen (0x39D858). A wide view would see them appear only as they reach
// the edge of the original screen. With a margin set, those tests accept what
// the wider view can show (the last one the same way). The code is the
// game's own and is left alone: the tests are changed by hooks (see
// cpu/tlcs900/instruction_hook.h) that set the flags the comparison produced,
// so they work the same in interpreted and recompiled code. With no margin they
// do nothing.
//
// Each of those tests is also widened vertically, by WIDE_EASE_MARGIN pixels
// (k2ge_render.h), for the picture's vertical camera easing (see
// Interpolator::ease_at): it uncovers that much above and below the screen.
//
// The two flapping birds of the fifth zone's flight (level 8) have a test of their
// own, which is widened the same way (see BIRD_WAKE in the .cpp), so they start
// beyond the picture and fly in instead of appearing inside it.
//
// Unlike the picture, this reaches the game: objects update a little earlier,
// and the sprite table gets longer by what lies in the new margins (tens of
// sprites at most, of its 64), which costs the game a little time. A recording
// therefore replays the same only in the aspect it was made in.
//
// Specific to this cartridge: the addresses belong to its code.
#pragma once

namespace ngpc {

// `extra` is how many pixels beyond each side of the screen the view shows
// (0 for the stock screen).
void set_wide_sprite_margin(int extra);

// Where the camera is locked (see wide_arena in level_map.h) the game keeps
// the player to the screen: his x on screen must stay between 12 and 148. The
// first zone's boss turns round at 72 pixels beyond the screen's edges. With
// `active` set, a wide view with `left` and `right` pixels beyond the screen
// gives both of them that much more room, so the player may go to the edges of
// the view and the boss crosses all of it. Call it every frame (from what the
// last frame left in RAM); it does nothing without a margin.
void set_wide_arena(bool active, int left, int right);

} // namespace ngpc
