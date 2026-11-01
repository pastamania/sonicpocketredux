#include "k2ge_interp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace k2ge_render {

namespace {

constexpr int OBJECT_SNAP = 64;    // an object further than this from where it was has jumped
constexpr int PLANE_SNAP = 64;
constexpr int MAX_STEP_FRAMES = 3;
constexpr int REMEMBER_TICKS = 8;   // how long an object that left the list is remembered
constexpr size_t REMEMBER_MAX = 32;
constexpr double EASE_STAGE = 0.5;  // share of the gap each filter stage closes a frame: rest in about 5 frames
constexpr double EASE_MAX_LAG = WIDE_EASE_MARGIN; // pixels the smoothed camera may trail the game's

int wrapped(int d)
{
	d &= 0xff;
	return d >= 128 ? d - 256 : d;
}

} // namespace


void Interpolator::Track::update(const int *value, int count, int wrapping, int snap_distance, int64_t now,
		int step_frames)
{
	if (!started)
	{
		to.assign(value, value + count);
		delta.assign(count, 0.0f);
		t_to = now;
		len = 1;
		started = true;
		return;
	}
	if (std::equal(to.begin(), to.end(), value))
		return;

	const double p = std::clamp(static_cast<double>(now - t_to) / len, 0.0, 1.0);
	std::vector<float> change(count);
	bool snap = false;
	// Per-line values that change shape from one frame to the next (a wave
	// effect, a split that moves) cannot be moved line by line.
	for (int i = 1; i < count && wrapping == count; i++)
		snap |= (value[i] == value[i - 1]) != (to[i] == to[i - 1]);
	for (int i = 0; i < count; i++)
	{
		const int d = i < wrapping ? wrapped(value[i] - to[i]) : value[i] - to[i];
		change[i] = static_cast<float>(d + (1.0 - p) * delta[i]);
		snap |= std::abs(d) > snap_distance;
	}
	delta = snap ? std::vector<float>(count, 0.0f) : change;
	len = step_frames > 0 ? step_frames : static_cast<int>(std::clamp<int64_t>(now - t_to, 1, MAX_STEP_FRAMES));
	to.assign(value, value + count);
	t_to = now;
}


float Interpolator::Track::trailing(int i, double t) const
{
	const double p = std::clamp((t - t_to) / len, 0.0, 1.0);
	return static_cast<float>((1.0 - p) * delta[i]);
}


void Interpolator::begin_tick(const ngpc::SpriteTable &table, int camera_dx, int camera_dy)
{
	// Objects and the camera move on the same game tick, so one glide length,
	// the time since the previous tick, serves for all of them.
	const int64_t gap = m_tick_frame < 0 ? 2 : m_now - m_tick_frame;
	m_tick_len = static_cast<int>(std::clamp<int64_t>(gap, 1, MAX_STEP_FRAMES));
	m_tick_frame = m_now;

	// Follow each object from earlier ticks by its owner: the ones in the last
	// tick's list and the ones remembered from before. The call site is tried
	// first (an owner can have several objects); an object whose call site
	// changed (an effect that switches routines) is the same owner's nearest
	// one.
	++m_ticks;
	std::vector<Object> pool = m_objects;
	pool.insert(pool.end(), m_lost.begin(), m_lost.end());
	const size_t count = table.entries.size();
	std::vector<int> from(count, -1);
	std::vector<char> claimed(pool.size(), 0);
	for (size_t e = 0; e < count; e++)
	{
		const ngpc::SpriteEntry &entry = table.entries[e];
		for (size_t j = 0; j < pool.size(); j++)
			if (!claimed[j] && pool[j].owner == entry.owner && pool[j].caller == entry.caller &&
					pool[j].ordinal == entry.ordinal)
			{
				from[e] = static_cast<int>(j);
				claimed[j] = 1;
				break;
			}
	}
	for (size_t e = 0; e < count; e++)
	{
		if (from[e] >= 0)
			continue;
		const ngpc::SpriteEntry &entry = table.entries[e];
		int best = -1, best_distance = OBJECT_SNAP + 1;
		for (size_t j = 0; j < pool.size(); j++)
		{
			if (claimed[j] || pool[j].owner != entry.owner)
				continue;
			const int distance = std::max(std::abs(entry.x - pool[j].x), std::abs(entry.y - pool[j].y));
			if (distance < best_distance)
			{
				best = static_cast<int>(j);
				best_distance = distance;
			}
		}
		if (best >= 0)
		{
			from[e] = best;
			claimed[best] = 1;
		}
	}

	// The HUD's task, found by the code that added its objects.
	std::vector<uint32_t> hud_owners;
	for (const ngpc::SpriteEntry &entry : table.entries)
		if (ngpc::is_hud_caller(entry.caller) && std::find(hud_owners.begin(), hud_owners.end(), entry.owner) == hud_owners.end())
			hud_owners.push_back(entry.owner);

	std::vector<Object> next(count);
	for (size_t e = 0; e < count; e++)
	{
		const ngpc::SpriteEntry &entry = table.entries[e];
		Object &object = next[e];
		object.hud = std::find(hud_owners.begin(), hud_owners.end(), entry.owner) != hud_owners.end();
		object.owner = entry.owner;
		object.caller = entry.caller;
		object.ordinal = entry.ordinal;
		object.x = entry.x;
		object.y = entry.y;
		object.t_to = m_now;
		object.len = m_tick_len;
		object.tick = m_ticks;
		object.camera_x = m_camera_x;
		object.camera_y = m_camera_y;
		if (from[e] >= 0)
		{
			const Object &old = pool[from[e]];
			const int step_x = entry.x - old.x, step_y = entry.y - old.y;
			const int absent = static_cast<int>(m_ticks - old.tick);   // ticks since it was last seen
			if (std::max(std::abs(step_x), std::abs(step_y)) <= OBJECT_SNAP)
			{
				if (absent <= 1)
				{
					const double p = std::clamp(static_cast<double>(m_now - old.t_to) / old.len, 0.0, 1.0);
					object.dx = static_cast<float>(step_x + (1.0 - p) * old.dx);
					object.dy = static_cast<float>(step_y + (1.0 - p) * old.dy);
				}
				else if (step_x + (m_camera_x - old.camera_x) == 0 && step_y + (m_camera_y - old.camera_y) == 0 &&
						(m_camera_x != old.camera_x || m_camera_y != old.camera_y))
				{
					// It was not shown while it was away, but it moved exactly
					// as the scenery did: it is part of the world.
					object.dx = static_cast<float>(-camera_dx);
					object.dy = static_cast<float>(-camera_dy);
				}
				else
				{
					// Not shown while away, so there is no picture to carry on
					// from: it moves by its share of the distance it covered.
					object.dx = static_cast<float>(step_x) / absent;
					object.dy = static_cast<float>(step_y) / absent;
				}
			}
		}
		else
		{
			// Nothing to follow: an object that has just appeared. Most are
			// part of the world, so it moves as the scenery does.
			if (std::max(std::abs(camera_dx), std::abs(camera_dy)) <= PLANE_SNAP)
			{
				object.dx = static_cast<float>(-camera_dx);
				object.dy = static_cast<float>(-camera_dy);
			}
		}
	}

	// What dropped out of the list is kept for a while in case it returns.
	std::vector<Object> lost;
	for (size_t j = 0; j < pool.size() && lost.size() < REMEMBER_MAX; j++)
		if (!claimed[j] && m_ticks - pool[j].tick < REMEMBER_TICKS)
			lost.push_back(pool[j]);
	m_lost = std::move(lost);
	m_objects = std::move(next);
	m_slot_object = table.entry_of_slot;
	m_serial = table.serial;
}


void Interpolator::push(const Frame &frame, const ngpc::SpriteTable *sprites, const WideScene *wide)
{
	m_frame = frame;
	m_have_frame = true;
	++m_now;

	// A wide view needs each line's world x, not wrapped at 256: the level's
	// world x on the first line, and how far each line's registers differ.
	if (wide && wide->active)
	{
		m_wide = *wide;
		for (int line = 0; line < LINES; line++)
		{
			m_frame.line[line].wx2 = wide->world_x + wrapped(frame.line[line].s2x - frame.line[0].s2x);
			m_frame.line[line].wx1 = wide->world_x + wrapped(frame.line[line].s1x - frame.line[0].s2x);
			// The window of plane 1 moves with the plane's scroll, and with the
			// scroll of its own row if the rows are windowed one by one.
			if (wide->p1_rows)
			{
				const int ring_row = ((frame.line[line].s1y + line) >> 3) & 31;
				m_frame.line[line].p1_lo = wide->p1_row_lo[ring_row] == WideScene::HIDDEN_ROW ? WideScene::HIDDEN_ROW :
						wide->p1_row_lo[ring_row] - wrapped(frame.line[line].s1x - wide->p1_row_s1x[ring_row]);
			}
			else
				m_frame.line[line].p1_lo = wide->p1_lo - wrapped(frame.line[line].s1x - frame.line[0].s1x);
		}
	}
	else
		m_wide.active = false;

	// How far the level plane moved this step, for objects that have just
	// appeared.
	int camera_dx = 0, camera_dy = 0;
	if (m_planes[2].started)
	{
		camera_dx = wrapped(frame.line[0].s2x - m_planes[2].to[0]);
		camera_dy = wrapped(frame.line[0].s2y - m_planes[3].to[0]);
		if (std::max(std::abs(camera_dx), std::abs(camera_dy)) <= PLANE_SNAP)
		{
			m_camera_x += camera_dx;
			m_camera_y += camera_dy;
		}
	}

	bool tick = false;
	if (sprites && sprites->serial != m_serial)
	{
		begin_tick(*sprites, camera_dx, camera_dy);
		tick = true;
	}
	else if (!sprites)
	{
		// Not the game's own sprite table: leave the sprites where they are.
		m_objects.clear();
		m_lost.clear();
		m_slot_object.fill(-1);
		m_serial = 0;
	}

	int plane[4][LINES];
	for (int line = 0; line < LINES; line++)
	{
		plane[0][line] = frame.line[line].s1x;
		plane[1][line] = frame.line[line].s1y;
		plane[2][line] = frame.line[line].s2x;
		plane[3][line] = frame.line[line].s2y;
	}
	// A scroll change on a game tick glides for as long as the tick does.
	for (int k = 0; k < 4; k++)
		m_planes[k].update(plane[k], LINES, LINES, PLANE_SNAP, m_now, tick ? m_tick_len : 0);

	// Easing needs the level's maps (the rows it uncovers) and the game's own
	// account of its sprites (to move them with the level), and is not for a
	// title card or the act clear, whose planes are screen-fixed pictures.
	ease_step(frame, wide && wide->active && sprites && !wide->centred && !wide->p1_rows);
}


void Interpolator::ease_step(const Frame &frame, bool eligible)
{
	// The level plane's y scroll, summed so it does not wrap; a jump (a respawn)
	// starts the filter again where the game now is.
	const int y = frame.line[0].s2y;
	bool restart = !m_ease_have;
	if (m_ease_have)
	{
		const int d = wrapped(y - m_ease_last);
		if (std::abs(d) > PLANE_SNAP)
			restart = true;
		else
			m_ease_pos += d;
	}
	if (restart)
		m_ease_pos = y;
	m_ease_have = true;
	m_ease_last = y;

	// Where the picture shows the plane at this frame's start.
	const double shown = m_ease_pos - m_planes[3].trailing(0, static_cast<double>(m_now));
	if (!eligible || restart)
	{
		m_ease_a = m_ease_b = m_ease_prev = m_ease_cur = shown;
		m_ease_on = false;
		return;
	}
	m_ease_prev = m_ease_cur;
	m_ease_a += EASE_STAGE * (shown - m_ease_a);
	m_ease_b += EASE_STAGE * (m_ease_a - m_ease_b);
	m_ease_a = std::clamp(m_ease_a, shown - EASE_MAX_LAG, shown + EASE_MAX_LAG);
	m_ease_b = std::clamp(m_ease_b, shown - EASE_MAX_LAG, shown + EASE_MAX_LAG);
	m_ease_cur = m_ease_b;
	m_ease_on = true;
}


int Interpolator::ease_at(double s) const
{
	if (!m_ease_on || s >= MAX_STEP_FRAMES_DRAWN)    // as drawn: the game's own picture
		return 0;
	const double t = static_cast<double>(m_now) + s;
	const double trailing = m_planes[3].trailing(0, t);
	const double shown = m_ease_pos - trailing;
	// The smoothed scroll at this moment, carried on from the last two frames.
	const double smoothed = std::clamp(m_ease_cur + std::min(s, 1.0) * (m_ease_cur - m_ease_prev),
			shown - EASE_MAX_LAG, shown + EASE_MAX_LAG);
	// The level is shown at the smoothed scroll rounded once, not at the game's
	// rounded and moved by a rounded difference, which would flicker by a pixel.
	return static_cast<int>(std::lround(smoothed)) -
			(static_cast<int>(std::lround(m_ease_pos)) - static_cast<int>(std::lround(trailing)));
}


void Interpolator::scenery_shift(double s, int &dx, int &dy) const
{
	const double t = static_cast<double>(m_now) + s;
	dx = static_cast<int>(std::lround(m_planes[2].trailing(0, t)));
	dy = static_cast<int>(std::lround(m_planes[3].trailing(0, t)));
}


SpriteShift Interpolator::sprite_shift(double s, int left, int right, bool wide, int ease_y) const
{
	const double t = static_cast<double>(m_now) + s;
	SpriteShift shift;
	for (int i = 0; i < SPRITES; i++)
	{
		// Under a title card every sprite that is not a level object's is the card's.
		if (m_slot_object[i] < 0)
		{
			shift.card[i] = wide && m_wide.centred;
			continue;
		}
		const Object &object = m_objects[m_slot_object[i]];
		if (wide && m_wide.centred)
			shift.card[i] = std::find(m_wide.card_owner, m_wide.card_owner + m_wide.card_owner_count, object.owner) !=
					m_wide.card_owner + m_wide.card_owner_count;
		const double behind = 1.0 - std::clamp((t - object.t_to) / object.len, 0.0, 1.0);
		shift.dx[i] = -static_cast<int>(std::lround(behind * object.dx));
		shift.dy[i] = -static_cast<int>(std::lround(behind * object.dy));
		if (wide)
		{
			// The camera is eased down by `ease_y`, so the world moves up on the
			// screen, but the HUD (and the card) stays where it is.
			if (!object.hud && !shift.card[i])
				shift.dy[i] -= ease_y;
			shift.ref_x[i] = object.x;
			// The HUD belongs to the edges of the picture, not to the
			// stock screen in the middle of it.
			if (object.hud)
				shift.dx[i] += object.x < 80 ? -left : right;
		}
	}
	return shift;
}


void Interpolator::render(double s, uint32_t *out) const
{
	if (!m_have_frame)
		return;
	const double t = static_cast<double>(m_now) + s;

	std::array<LineRegs, LINES> regs = m_frame.line;
	for (int line = 0; line < LINES; line++)
	{
		auto shifted = [&](const Track &track, int value) {
			return static_cast<uint8_t>((value - static_cast<int>(std::lround(track.trailing(line, t)))) & 0xff);
		};
		regs[line].s1x = shifted(m_planes[0], m_frame.line[line].s1x);
		regs[line].s1y = shifted(m_planes[1], m_frame.line[line].s1y);
		regs[line].s2x = shifted(m_planes[2], m_frame.line[line].s2x);
		regs[line].s2y = shifted(m_planes[3], m_frame.line[line].s2y);
	}

	// A wide view: the picture is the level's own width, placed so that it
	// does not reach past the level's ends.
	View view = m_view;
	int left = 0, right = 0, hud_left = 0, hud_right = 0, ease = 0;
	const bool wide = m_wide.active && view.width > 160;
	if (wide)
	{
		for (int line = 0; line < LINES; line++)
		{
			regs[line].wx2 = m_frame.line[line].wx2 - static_cast<int>(std::lround(m_planes[2].trailing(line, t)));
			regs[line].wx1 = m_frame.line[line].wx1 - static_cast<int>(std::lround(m_planes[0].trailing(line, t)));
			regs[line].p1_lo = m_frame.line[line].p1_lo == WideScene::HIDDEN_ROW ? WideScene::HIDDEN_ROW :
					m_frame.line[line].p1_lo + static_cast<int>(std::lround(m_planes[0].trailing(line, t)));
		}
		wide_placement(regs[0].wx2, left, right);
		view.origin = -left;
		view.card_origin = -((m_view.width - 160) / 2);
		view.screen_only = false;
		// The HUD keeps to the edges of what is shown, not of the black.
		hud_left = std::max(0, left - std::max(0, m_wide.min_x - (regs[0].wx2 - left)));
		hud_right = std::max(0, right - std::max(0, (regs[0].wx2 + 160 + right) - m_wide.max_x));

		// The camera's vertical easing moves the level, and plane 1 with it
		// when it is a part of the level (it scrolls with it, or holds a boss).
		ease = ease_at(s);
		const bool plane1_moves = m_wide.plane1 || (m_wide.p1_window && !m_wide.p1_rows);
		for (int line = 0; line < LINES; line++)
		{
			regs[line].s2y = static_cast<uint8_t>((regs[line].s2y + ease) & 0xff);
			if (plane1_moves)
				regs[line].s1y = static_cast<uint8_t>((regs[line].s1y + ease) & 0xff);
		}
	}

	const SpriteShift shift = sprite_shift(s, wide ? hud_left : 0, wide ? hud_right : 0, wide, ease);
	render_frame(m_frame, regs.data(), view, &shift, out, wide ? &m_wide : nullptr);
}


void Interpolator::wide_placement(int world_x, int &left, int &right) const
{
	// The extra width is split either side of the screen, except that the
	// picture does not reach past the camera's range: near an end of the
	// level the extra goes to the other side. If the range is narrower than
	// the picture (a locked camera) the rest is left black.
	const int extra = m_view.width - 160;
	const int room_left = std::max(0, world_x - m_wide.min_x);
	const int room_right = std::max(0, m_wide.max_x - (world_x + 160));
	const int lowest = std::max(0, extra - room_right), highest = std::min(extra, room_left);
	left = lowest <= highest ? std::clamp(extra / 2, lowest, highest) : extra / 2;
	if (m_wide.arena)
		left = extra / 2;
	right = extra - left;
}


bool Interpolator::wide_extent(int &world_first, int &world_last) const
{
	if (!m_have_frame || !m_wide.active || m_view.width <= 160)
		return false;
	int left = 0, right = 0;
	wide_placement(m_frame.line[0].wx2, left, right);
	world_first = m_frame.line[0].wx2 - left;
	world_last = m_frame.line[0].wx2 + 160 + right;
	return true;
}

} // namespace k2ge_render
