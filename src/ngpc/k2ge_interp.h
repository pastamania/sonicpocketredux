// Draws pictures between emulated frames so the host can present more often
// than the machine's 60 Hz.
//
// The game moves things in steps: once per game tick (every second frame) it
// rebuilds its sprite list and moves the camera. After each tick, positions
// glide from where they were being shown to where the game now has them over
// the length of a tick, so everything is shown about one tick late.
//
// An object that drops out of the list for a few ticks and comes back (the
// player flashing when hurt or invincible, anything that blinks) is the same
// object: it is remembered for a while, and when it returns it moves on at the
// pace it had, not as something new.
//
// Scroll planes are tracked per plane and axis from the scroll registers.
// Sprites are tracked per object, as the game itself groups them (see
// sprite_trace.h): every hardware sprite of an object gets the same shift, and
// an object is followed from tick to tick by the task that owns it, so
// animation changes, reshuffled sprite slots and identical-looking tiles
// cannot confuse it. Sprites the game did not build through its list are left
// where the game put them.
#pragma once

#include "ngpc/k2ge_render.h"
#include "ngpc/sprite_trace.h"

#include <array>
#include <cstdint>
#include <vector>

namespace k2ge_render {

class Interpolator
{
public:
	// Far enough through a frame that everything has arrived.
	static constexpr double MAX_STEP_FRAMES_DRAWN = 4.0;

	explicit Interpolator(const View &view) : m_view(view) {}

	// Changes the shape of the pictures drawn from here on. Nothing tracked
	// depends on it.
	void set_view(const View &view) { m_view = view; }

	// Whether pictures between the game's ticks place the level and the sprites to 1/SUBPIXELS
	// of a pixel (the default) or to whole pixels, as the game does.
	void set_subpixel(bool on) { m_subpixel = on; }

	// Adds the frame the machine has just finished, with the game's account
	// of its sprite table (SpriteTrace::match), or null if there is none, and
	// the level's tile maps for a wide view (LevelMap::build), if there are
	// any: while they are active a view wider than the screen shows them.
	void push(const Frame &frame, const ngpc::SpriteTable *sprites, const WideScene *wide = nullptr);

	// Draws the picture `s` (0..1) of the way through the frame last pushed
	// into `out` (view.width * LINES pixels).
	void render(double s, uint32_t *out) const;

	// Draws the frame last pushed as the game drew it, with only the view's
	// shape and the wide scene applied (no in-between shifts).
	void render_as_drawn(uint32_t *out) const { render(MAX_STEP_FRAMES_DRAWN, out); }

	// The world x of the first and last pixel (exclusive) a wide view shows of
	// the frame last pushed, as drawn; false if it is not a wide view. For
	// tooling: the scene must hold the tiles of all of it.
	bool wide_extent(int &world_first, int &world_last) const;

	// How far each sprite is moved from where the game put it in that picture.
	// In a wide view `left` and `right` are the extra width either side of the
	// screen, and HUD objects are moved out to the edges.
	// `ease_y` is the vertical easing of the picture (see ease_at), which moves
	// every sprite but the HUD and the title card's.
	SpriteShift sprite_shift(double s, int left = 0, int right = 0, bool wide = false, int ease_y = 0) const;

	// How far the level's scroll plane is displaced from where the game put it
	// in that picture (the shift of anything fixed in the world, x and y).
	void scenery_shift(double s, int &dx, int &dy) const;

	// Vertical camera easing, for wide views. The game's camera sits still until
	// the player leaves a window and then moves at its full catch-up speed, and
	// stops dead: horizontally it follows the player's speed, so that axis ramps
	// up and down by itself. A smoothed camera follows the game's (two first-order
	// filters, so its speed is continuous: it picks up and comes to rest over a few
	// frames) and the whole picture is moved by the difference: the level, and every
	// sprite but the HUD and the title card's. The difference is at most WIDE_EASE_MARGIN pixels (see wide_sprites.h for the sprites the game must keep there).
	// This is how far, in pixels, the picture's scroll is moved down at `s` of the
	// way through the last frame (0 when not easing: a view of the stock width, no
	// level, a title card or the act clear, or the picture as drawn).
	int ease_at(double s) const;

private:
	struct Track
	{
		std::vector<int> to;        // newest position
		std::vector<float> delta;   // how far the shown position trails `to` at t_to
		int64_t t_to = 0;           // frame that position arrived
		int len = 1;                // frames over which it is approached
		bool started = false;

		// The first `wrapping` coordinates wrap at 256. `step_frames` is the
		// glide length to use, or 0 to take the time since the last change.
		void update(const int *value, int count, int wrapping, int snap_distance, int64_t now, int step_frames);
		float trailing(int i, double t) const;   // distance behind `to` at time t
	};

	// A sprite object as of the last game tick, and where it is shown
	// relative to where the game put it.
	struct Object
	{
		uint32_t owner = 0, caller = 0;
		int ordinal = 0;
		int x = 0, y = 0;           // newest position
		float dx = 0, dy = 0;       // how far the shown position trails it at t_to
		int64_t t_to = 0;
		int len = 1;
		int64_t tick = 0;           // the game tick it was last in the list
		bool hud = false;           // belongs to the HUD task
		int camera_x = 0, camera_y = 0;   // where the camera was then (summed scroll steps)
	};

	void begin_tick(const ngpc::SpriteTable &table, int camera_dx, int camera_dy);
	void ease_step(const Frame &frame, bool eligible);
	// How the extra width of a wide view is split: `world_x` is the world x of the
	// screen's first pixel.
	void wide_placement(int world_x, int &left, int &right) const;

	View m_view;
	Frame m_frame;
	WideScene m_wide;
	int64_t m_now = -1;
	bool m_have_frame = false;
	std::vector<Object> m_objects;
	std::vector<Object> m_lost;                    // dropped out of the list recently
	int64_t m_ticks = 0;                           // game ticks seen
	bool m_subpixel = true;
	int m_camera_x = 0, m_camera_y = 0;            // the level plane's scroll, summed without wrapping
	std::array<int8_t, SPRITES> m_slot_object{};   // object of each sprite slot, -1 for none
	uint64_t m_serial = 0;                         // the sprite table the objects are from
	int64_t m_tick_frame = -1;
	int m_tick_len = 2;                            // frames between game ticks
	std::array<Track, 4> m_planes;                 // 1x, 1y, 2x, 2y
	bool m_ease_have = false;                      // m_ease_pos follows the level plane's y scroll
	bool m_ease_on = false;                        // this frame's picture is eased
	int m_ease_last = 0;                           // the level plane's y scroll last frame
	double m_ease_pos = 0;                         // that, summed without wrapping
	double m_ease_a = 0, m_ease_b = 0;             // the two stages of the filter
	double m_ease_prev = 0, m_ease_cur = 0;        // the smoothed scroll last frame and this one
};

} // namespace k2ge_render
