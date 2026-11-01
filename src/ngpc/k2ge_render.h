// K2GE line renderer that works from a captured frame instead of live video
// RAM, so the host can draw at a different width or between two frames.
//
// The drawing rules are those of k2ge.cpp (adapted from MAME's k1ge.cpp). At
// the stock view (width 160, origin 0, no shift) the output is identical to
// k2ge's own framebuffer.
#pragma once

#include <array>
#include <cstdint>
#include <vector>
// INT32_MIN

namespace k2ge_render {

constexpr int LINES = 152;
constexpr int MAX_WIDTH = 320;     // the widest view (a scroll plane's own ring is 256 pixels)
constexpr int SPRITES = 64;

// Registers the picture depends on that a game may change between lines.
struct LineRegs
{
	uint8_t po_h = 0, po_v = 0;        // sprite offset
	uint8_t s1x = 0, s1y = 0;          // scroll plane 1
	uint8_t s2x = 0, s2y = 0;          // scroll plane 2
	uint8_t pf = 0;                    // plane priority (bit 7)
	uint8_t bg = 0;                    // background colour select
	uint8_t oow = 0;                   // outside-of-window colour
	uint8_t wba_h = 0, wba_v = 0;      // window origin
	uint8_t wsi_h = 0xff, wsi_v = 0xff; // window size
	// Set by the host for a wide view (see WideScene): the world x, not
	// wrapped at 256, of screen x 0 for each scroll plane on this line.
	int wx1 = 0, wx2 = 0;
	// Set by the host for a wide scene with a plane 1 window (see WideScene):
	// the screen x where this line's window of 256 pixels of plane 1 starts.
	int p1_lo = 0;
};

using Palette = std::array<uint8_t, 0x200>;

// One finished frame: the video RAM the visible lines were drawn from, the
// per-line registers, and the palette RAM in force on each line.
struct Frame
{
	std::array<uint8_t, 0x4000> vram{};
	std::array<LineRegs, LINES> line{};
	std::array<uint8_t, LINES> palette_index{};
	std::vector<Palette> palettes;         // distinct palette RAM contents

	const Palette &palette(int y) const { return palettes[palette_index[y]]; }
};

// The tile maps of a level for the whole width of a wide view. The hardware
// keeps only 256 pixels of each plane's map, and the game fills it just in
// time, so what lies beyond the screen is not there to be drawn. The host
// builds it from the level data instead (see level_map.h).
// The scene holds the columns of the widest view at any placement: the extra
// width (up to 110 pixels, 14 columns) may all lie to one side of the screen's
// 21 columns.
constexpr int WIDE_LEFT_COLS = 14; // columns held to the left of the screen's first
constexpr int WIDE_COLS = 52;      // tile columns held: 416 pixels
struct WideScene
{
	bool active = false;
	bool plane1 = false;           // plane 1 scrolls with plane 2, so it too comes from the scene
	// A screen laid out for 160 pixels sits over the level (the zone's title
	// card). The level is placed as it will be once the card is gone, and the
	// card, its plane 1 and the sprites of its tasks, goes in the middle of the
	// view (View::card_origin), so nothing moves when the card leaves.
	bool centred = false;
	static constexpr int CARD_OWNERS = 4;
	uint32_t card_owner[CARD_OWNERS] = {};   // records of the card's tasks, which own its sprites
	int card_owner_count = 0;
	// The card has not started to move. Its plane 1 holds pieces waiting beyond
	// the 160 pixels of the screen to slide in, which a wider view would show
	// standing there: until the card moves, only its 160 pixels are drawn.
	bool card_hold = false;
	// The camera is locked in an act's end or a boss arena (see wide_arena):
	// the view is centred on the screen and shows the level beyond it.
	bool arena = false;
	// Plane 1 is drawn from the hardware's 256-pixel ring and holds a block (a
	// boss: the game moves it by scrolling the plane). A view wider than the
	// screen sees the ring wrap: the part of the block that is beyond one edge
	// shows again at the other. With this set only a window of 256 pixels of
	// screen x is drawn, starting at p1_lo (on the first line), which is the
	// lap of the ring the block is in, so every column shows once, where it is.
	bool p1_window = false;
	int p1_lo = 0;
	int world_x = 0;               // world x of screen x 0 on the first line
	int level_width = 0;           // pixels
	// The part of the world the camera can show: the level's edge margins and
	// anything outside a locked camera's range are left black.
	int min_x = 0, max_x = 0;
	int first_col = 0;             // world tile column of entry index 0
	int row[32] = {};              // world tile row of each ring row (the rows the hardware map holds)
	// [plane 1, plane 2][ring row][world tile column - first_col]. Rows are
	// those of the hardware map, so the scroll registers pick them as usual.
	uint16_t entry[2][32][WIDE_COLS];
};

// Which part of the 256-pixel-wide picture is output. `origin` is the screen x
// of output column 0 (0 for the stock 160-pixel view, -48 for a 256-pixel
// view centred on it).
struct View
{
	int width = 160;
	int origin = 0;
	// Draw only the stock 160-pixel screen (screen x 0..159) and leave the rest
	// of the width black: the picture of a game screen that has nothing to show
	// beyond the machine's own screen, centred in a wider window.
	bool screen_only = false;
	// The screen x of output column 0 for what belongs to a screen made for 160
	// pixels (see WideScene::centred); `origin` is the level's.
	int card_origin = 0;
};

// The shapes of picture the host offers. All keep the machine's 152 lines.
enum class Aspect { Original, Wide16x10, Wide16x9 };
constexpr int ASPECT_COUNT = 3;

constexpr int aspect_width(Aspect aspect)
{
	// 152 lines * 16/10 = 243.2 and * 16/9 = 270.2
	return aspect == Aspect::Wide16x10 ? 243 : aspect == Aspect::Wide16x9 ? 270 : 160;
}

const char *aspect_name(Aspect aspect);              // "original", "16:10", "16:9"
bool parse_aspect(const char *text, Aspect &aspect); // those names, plus "16x10", "16x9"
Aspect next_aspect(Aspect aspect);                   // cycles through the three

// The view for an aspect: the stock screen centred in the full width.
View aspect_view(Aspect aspect);

// Per-sprite displacement applied after the sprite's position is resolved;
// used to place sprites between two frames.
struct SpriteShift
{
	static constexpr int NO_REF = INT32_MIN;

	int dx[SPRITES] = {};
	int dy[SPRITES] = {};
	// The screen x, not wrapped at 256, of the object a sprite belongs to, or
	// NO_REF. A sprite's own x is 8 bits, which is ambiguous in a view wider
	// than 256 pixels; with this the sprite is placed at the position nearest
	// to the object.
	int ref_x[SPRITES];
	// A sprite of the title card: placed by View::card_origin, not the level's.
	bool card[SPRITES] = {};

	SpriteShift() { for (int &x : ref_x) x = NO_REF; }
};

// Draws one line (0..151) into `out[view.width]` as 0xAARRGGBB.
// With an active `wide` scene the scroll planes are drawn from it, at the
// world x in `regs` (wx1, wx2), instead of from the video RAM's 256-pixel map.
void render_line(const uint8_t *vram, const uint8_t *palette, const LineRegs &regs, int line,
		const View &view, const SpriteShift *shift, uint32_t *out, const WideScene *wide = nullptr);

// Draws a whole frame with its own registers, or with `regs` substituted (for
// scroll positions between frames). `out` holds view.width * LINES pixels.
void render_frame(const Frame &frame, const LineRegs *regs, const View &view,
		const SpriteShift *shift, uint32_t *out, const WideScene *wide = nullptr);

} // namespace k2ge_render
