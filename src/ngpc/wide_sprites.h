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
// A wide picture shares its extra width evenly either side of the screen, except near an end
// of the level, where the picture does not reach past the camera's range and all of it lies on
// one side: at the very start of a level up to twice the margin is to the right of the screen,
// and at the very end to the left (see k2ge_render::wide_split). The three tests above are
// widened by how far the view really reaches on each side (track_wide_view), not by the even
// share, or an object there would not be drawn until it was well inside the picture: the first
// speed booster of Aero Base (zone 5, act 2: level 9) stands 232 pixels from the screen's left
// at the start, and did not appear until 224, about 45 pixels short of the picture's edge.
//
// Each of those tests is also widened vertically, by WIDE_EASE_MARGIN pixels
// (k2ge_render.h), for the picture's vertical camera easing (see
// Interpolator::ease_at): it uncovers that much above and below the screen.
//
// The last fight's hull (level 12) is drawn by the game's code one half at a time, the
// half that is on the screen; the two tests that say which half is off it are given the
// margin as well (see HULL_LEFT in the .cpp), so the half in the margin is drawn too.
//
// The cutscene that opens the last level has Robotnik run across the screen and ends him
// 16 pixels beyond its right edge (0x39EBDE, see ACTOR_EXIT in the .cpp), which in a wide view
// is in the middle of the picture: with a margin he runs to beyond its widest right edge,
// which is twice the margin (at the start of a level the extra width is all on the right).
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

namespace k2ge_render {
struct WideScene;
}

namespace ngpc {

// `extra` is how many pixels beyond each side of the screen the view shows
// (0 for the stock screen).
void set_wide_sprite_margin(int extra);

// Tells the sprite tests how far the view reaches beyond each side of the screen: `scene` is
// what the host built from the frame it has just shown (null when it is not a level, which
// the screen's centre is shown as it is), and `extra` is the picture's width less 160. Call it
// after every frame, with the margin set; it does nothing without one. It follows the camera's
// last step, so that the view the next tick's sprites will be shown in is covered whichever way
// the camera goes.
void track_wide_view(const k2ge_render::WideScene *scene, int extra);

// How far the sprite tests now take the view to reach beyond the screen's left and right edges
// (0, 0 with no margin). For tests.
void wide_view_reach(int &left, int &right);

// Where the camera is locked (see wide_arena in level_map.h) the game keeps
// the player to the screen: his x on screen must stay between 12 and 148. The
// first zone's boss turns round at 72 pixels beyond the screen's edges. With
// `active` set, a wide view with `left` and `right` pixels beyond the screen
// gives both of them that much more room, so the player may go to the edges of
// the view and the boss crosses all of it. Call it every frame (from what the
// last frame left in RAM); it does nothing without a margin.
void set_wide_arena(bool active, int left, int right);

} // namespace ngpc
