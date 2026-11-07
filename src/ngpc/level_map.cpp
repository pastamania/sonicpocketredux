#include "ngpc/level_map.h"

#include "ngpc/machine.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdlib>

namespace ngpc {

namespace {

constexpr uint32_t ROM_BASE = 0x200000;
constexpr int BLANK = 0x01ff;            // the blank tile the game fills empty map cells with
constexpr int SCREEN_ROWS = 20;          // tile rows touched by the 152-line screen
constexpr int SCREEN_COLS = 21;          // tile columns touched by the 160-pixel screen
constexpr double AGREE = 0.9;            // share of tiles that must match for a level to be recognised
constexpr int MIN_TILES = 200;           // and how many must lie inside the level for that to count

int floor_div(int a, int b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int mod(int a, int b)
{
	return ((a % b) + b) % b;
}

} // namespace


LevelRam read_level_ram(Machine &machine, bool finale_before)
{
	auto word = [&](uint32_t address) { return machine.read_byte(address) | (machine.read_byte(address + 1) << 8); };
	LevelRam ram;
	ram.header = static_cast<uint32_t>(word(LEVEL_HEADER_PTR)) | (static_cast<uint32_t>(word(LEVEL_HEADER_PTR + 2)) << 16);
	ram.camera_x = word(LEVEL_CAMERA_X);
	ram.camera_y = word(LEVEL_CAMERA_Y);
	ram.camera_x_start = word(LEVEL_CAMERA_X_START);
	ram.camera_x_end = word(LEVEL_CAMERA_X_END);
	ram.level = machine.read_byte(LEVEL_NUMBER);
	ram.player_x = static_cast<int16_t>(word(PLAYER_TASK + 0x12)) - ram.camera_x;

	// The title card and the bosses are tasks of their own: walk the (circular)
	// list.
	uint32_t record = TASK_LIST_HEAD;
	bool pod_hit = false, space_task = false;
	for (int count = 0; count < 64 && record >= 0x4000 && record < 0x7000; count++)
	{
		const uint32_t function = machine.read_byte(record + TASK_FUNCTION) |
				(machine.read_byte(record + TASK_FUNCTION + 1) << 8) |
				(machine.read_byte(record + TASK_FUNCTION + 2) << 16);
		if (function >= TITLE_CARD_FIRST && function < TITLE_CARD_END)
		{
			ram.title_card = true;
			if (ram.card_owner_count < CARD_TASKS)
				ram.card_owner[ram.card_owner_count++] = record;
		}
		if (function >= ACT_CLEAR_FIRST && function < ACT_CLEAR_END)
			ram.act_clear = true;
		if ((function >= ENDING_FIRST && function < ENDING_END) ||
				(function >= ENDING_SCENES_FIRST && function < ENDING_SCENES_END))
			ram.ending = true;
		if (function == LAST_BOSS_POD_HIT)
			pod_hit = true;
		if (function >= CHAOTIC_SPACE_FIRST && function < CHAOTIC_SPACE_END)
			space_task = true;
		if (ram.level == CHAOTIC_SPACE_LEVEL && ram.boss_count < BOSS_TASKS &&
				std::find(std::begin(CHAOTIC_SPACE_MACHINE), std::end(CHAOTIC_SPACE_MACHINE), function) != std::end(CHAOTIC_SPACE_MACHINE))
			ram.boss_x[ram.boss_count++] = static_cast<int16_t>(word(record + 0x12)) - static_cast<int16_t>(word(CHAOTIC_SPACE_PAN));
		const bool boss_code = (function >= BOSS_CODE_FIRST && function < BOSS_CODE_END) ||
				(function >= BOSS5_CODE_FIRST && function < BOSS5_CODE_END);
		if (boss_code && ram.boss_count < BOSS_TASKS)
		{
			const int x = static_cast<int16_t>(word(record + 0x12)) - ram.camera_x;
			if (std::abs(x) < BOSS_X_LIMIT)
				ram.boss_x[ram.boss_count++] = x;
		}
		record = word(record + TASK_NEXT);
		if (record == TASK_LIST_HEAD)
			break;
	}
	// The finale lasts from the pod's hit through gaps with no task of its own (the
	// controller ends, a fade runs, the ending starts), until the level is left or
	// started again (the title card).
	ram.space = ram.level == CHAOTIC_SPACE_LEVEL && (space_task || ram.title_card);
	ram.finale = ram.level == FINAL_LEVEL && !ram.title_card && (pod_hit || finale_before);
	if (ram.finale)
		ram.ending = true;
	return ram;
}


bool arena_level(const LevelRam &ram)
{
	// Every level that locks the camera: the end of each act 1 (0, 2, 4, 6, 10)
	// and the boss fights (1, 3, 7, 9, 11 and the final one, 12). Not 5 (the
	// third zone's boss: the camera never narrows) or 8 (no lock).
	static constexpr bool ARENA_LEVEL[13] = { true, true, true, true, true, false, true, true, false, true, true, true, true };
	return ram.header != 0 && ram.level >= 0 && ram.level < 13 && ARENA_LEVEL[ram.level];
}


bool plane1_block_level(const LevelRam &ram)
{
	return ram.header != 0 && ram.level == 8;
}


bool wide_arena(const LevelRam &ram)
{
	// Locked: the camera may move less than a screen's width (the end of an
	// act: none; the second zone's boss: 80 pixels).
	// Chaotic Space keeps Super Sonic to the screen all the way through (once the card is gone),
	// with no level around the picture, so the player gets the view's width there too.
	if (ram.space)
		return !ram.title_card && !ram.ending;
	return arena_level(ram) && !ram.ending && ram.camera_x_end > 0 && ram.camera_x_end - ram.camera_x_start < 160;
}


uint16_t LevelMap::rom_word(uint32_t address) const
{
	if (address < ROM_BASE || address - ROM_BASE + 1 >= m_rom.size())
		return 0;
	const uint32_t offset = address - ROM_BASE;
	return static_cast<uint16_t>(m_rom[offset] | (m_rom[offset + 1] << 8));
}


bool LevelMap::parse(uint32_t address, Header &header) const
{
	if (address < ROM_BASE || address - ROM_BASE + 0x20 > m_rom.size())
		return false;
	header.width = rom_word(address);
	header.height = rom_word(address + 2);
	if (header.width < 1 || header.width > 1024 || header.height < 1 || header.height > 1024)
		return false;
	auto pointer = [&](uint32_t at) {
		return static_cast<uint32_t>(rom_word(at)) | (static_cast<uint32_t>(rom_word(at + 2)) << 16);
	};
	header.layout[1] = pointer(address + 0x0A);
	header.blocks = pointer(address + 0x0E);
	header.layout[0] = pointer(address + 0x1A);
	const uint32_t layout_bytes = static_cast<uint32_t>(header.width) * header.height * 2;
	for (uint32_t layout : { header.layout[0], header.layout[1] })
		if (layout < ROM_BASE || layout - ROM_BASE + layout_bytes > m_rom.size())
			return false;
	return header.blocks >= ROM_BASE && header.blocks - ROM_BASE < m_rom.size();
}


uint16_t LevelMap::entry(const Header &header, int plane, int column, int row) const
{
	if (column < 0 || row < 0 || column / 4 >= header.width || row / 4 >= header.height)
		return BLANK;
	const uint16_t block = rom_word(header.layout[plane] + 2 * ((row / 4) * header.width + column / 4));
	return rom_word(header.blocks + block * 32 + ((row % 4) * 4 + column % 4) * 2);
}


uint16_t LevelMap::scene_entry(const Header &header, int plane, int column, int row, bool extend) const
{
	if (extend && plane == 1 && m_last_column >= 0 && column / 4 > m_last_column && column / 4 < header.width)
		column = m_last_column * 4 + 3;
	return entry(header, plane, column, row);
}


void LevelMap::card_state(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene)
{
	const k2ge_render::LineRegs &top = frame.line[0];
	scene.centred = ram.title_card;
	scene.card_owner_count = ram.card_owner_count;
	for (int j = 0; j < ram.card_owner_count; j++)
		scene.card_owner[j] = ram.card_owner[j];
	if (!ram.title_card)
		m_card_seen = false;
	else if (!m_card_seen)
	{
		m_card_seen = true;
		m_card_moved = false;
		m_card_s1x = top.s1x;
	}
	else if (top.s1x != m_card_s1x)
		m_card_moved = true;
	scene.card_hold = ram.title_card && !m_card_moved;
}


bool LevelMap::build_space(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene)
{
	// Nothing of a level's tiles is held for it, so nothing is left over for the next level.
	m_header = 0;
	m_row_offset = -1;
	m_last_world_x = -1;
	m_edits.clear();
	for (RowTrack &track : m_rows)
		track = RowTrack();

	scene.active = true;
	scene.ring = true;
	scene.plane1 = false;
	scene.arena = false;
	scene.act_clear = false;
	scene.p1_window = false;
	scene.p1_rows = false;
	// The machine's body is a block of plane 1's tiles that the game moves by scrolling the plane,
	// as a boss's is in an arena: the ring is 256 pixels and a view up to 270, so the block, near an
	// edge, would show again at the other (see p1_block_window). Not under the card, whose text is
	// plane 1's too. The plane is scrolled as a whole.
	bool uniform = true;
	for (int line = 0; line < k2ge_render::LINES; line++)
		if (frame.line[line].s1x != frame.line[0].s1x)
			uniform = false;
	if (ram.title_card || !uniform)
		m_p1_known = false;
	else
		p1_block_window(frame, ram, scene, false);
	// The camera's x is only a place to count the scroll from; the ring has no ends.
	scene.world_x = ram.camera_x;
	scene.level_width = 0;
	scene.min_x = -(1 << 20);
	scene.max_x = 1 << 20;
	scene.first_col = 0;
	card_state(frame, ram, scene);
	m_agreement = 1.0;   // the picture is the hardware's own: there is nothing to disagree with
	return true;
}


void LevelMap::p1_block_window(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene,
		bool follow_player)
{
	using namespace k2ge_render;
	const LineRegs &top = frame.line[0];
	bool occupied[32] = {};
	int count = 0;
	for (int column = 0; column < 32; column++)
		for (int row = 0; row < 32 && !occupied[column]; row++)
		{
			const int at = 0x1000 + row * 64 + column * 2;
			const int tile = frame.vram[at] | (frame.vram[at + 1] << 8);
			if (tile != 0 && tile != BLANK)
				occupied[column] = true;
		}
	for (bool o : occupied)
		count += o;
	// The block is what lies outside the longest empty run of columns.
	int gap_start = -1, gap = 0;
	for (int start = 0; start < 32 && count > 0 && count < 32; start++)
	{
		if (occupied[start] || !occupied[(start + 31) % 32])
			continue;   // not the first column of an empty run
		int run = 0;
		while (!occupied[(start + run) % 32])
			run++;
		if (run > gap)
		{
			gap = run;
			gap_start = start;
		}
	}
	if (gap_start >= 0 && gap >= 4)   // a block, not a plane filled all round
	{
		const int first = (gap_start + gap) % 32, width = 32 - gap;
		const int middle = (first * 8 + width * 4) & 255;    // in the ring
		const int scroll = top.s1x;
		int centre = mod(middle - scroll, 256);             // on screen, one lap of several
		// The lap is the one a boss is in, if the game has one near the block;
		// otherwise the one nearest to where the block was.
		int best = 65;
		int chosen = centre;
		auto consider = [&](int anchor) {
			const int lap = centre + 256 * static_cast<int>(std::lround((anchor - centre) / 256.0));
			if (std::abs(lap - anchor) < best)
			{
				best = std::abs(lap - anchor);
				chosen = lap;
			}
		};
		for (int j = 0; j < ram.boss_count; j++)
			consider(ram.boss_x[j]);
		// The plane of the flight is where the player is (a few pixels to
		// its left: its middle was 9 pixels left of the player's x).
		if (follow_player)
			consider(ram.player_x - 9);
		if (best <= 64)
			centre = chosen;
		else
		{
			const int near = m_p1_known ? m_p1_centre : 80;
			centre += 256 * static_cast<int>(std::lround((near - centre) / 256.0));
		}
		m_p1_centre = centre;
		m_p1_known = true;
		scene.p1_window = true;
		scene.p1_lo = centre - 128;
	}
}


bool LevelMap::build(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene)
{
	using namespace k2ge_render;

	scene.active = false;
	m_agreement = 0;
	// The last boss's end, an ending or the credits: a picture made for the screen's
	// 160 pixels, with nothing of the level to show beyond it (the level's tiles are
	// still the last ones the level had to give). It is shown as every screen that is
	// not a level is, centred between black bars.
	if (ram.ending)
		return false;
	if (ram.space)
		return build_space(frame, ram, scene);
	Header header;
	if (!parse(ram.header, header))
		return false;
	if (ram.header != m_header)
	{
		m_header = ram.header;
		m_row_offset = -1;
		m_edits.clear();
		m_card_seen = false;
	}
	// (Every frame: the end fill of wide_levels.cpp may have put blocks in the
	// last columns since. It is a few columns from the end at most.)
	m_last_column = -1;
	for (int column = header.width - 1; column >= 0 && m_last_column < 0; column--)
		for (int row = 0; row < header.height; row++)
			if (rom_word(header.layout[1] + 2 * (row * header.width + column)) != 0)
			{
				m_last_column = column;
				break;
			}

	const LineRegs &top = frame.line[0];
	const int level_width = header.width * 32;

	// The scroll registers wrap at 256; the camera says which lap the level is on.
	const int world_x = top.s2x + 256 * floor_div(ram.camera_x - top.s2x + 128, 256);
	if (world_x < 0 || world_x > level_width)
		return false;
	const int first_ring_row = top.s2y >> 3;
	const int first_column = world_x >> 3;

	// The level's y counts up from the bottom: the top of the screen is this
	// many pixels down from the top of the level.
	const int expected_row = floor_div(header.height * 32 - 152 - ram.camera_y, 8);

	// World tile row of the screen's first ring row, for a ring row offset k.
	auto first_row_for = [&](int k) {
		const int base = mod(first_ring_row - k, 32);
		return base + 32 * static_cast<int>(std::lround((expected_row - base) / 32.0));
	};
	// How many of the tiles on screen the decoder gets right, and how many of
	// them lie inside the level.
	auto agreement = [&](int k, int &inside) {
		const int first_row = first_row_for(k);
		int agree = 0;
		inside = 0;
		for (int r = 0; r < SCREEN_ROWS; r++)
		{
			const int ring_row = (first_ring_row + r) & 31;
			for (int c = 0; c < SCREEN_COLS; c++)
			{
				const int column = first_column + c;
				if (column < 0 || column / 4 >= header.width || first_row + r < 0 || (first_row + r) / 4 >= header.height)
					continue;
				++inside;
				const int at = 0x1800 + ring_row * 64 + (column & 31) * 2;
				if (entry(header, 1, column, first_row + r) == (frame.vram[at] | (frame.vram[at + 1] << 8)))
					++agree;
			}
		}
		return agree;
	};

	int offset = m_row_offset;
	int inside = 0, agree = 0;
	if (offset >= 0)
		agree = agreement(offset, inside);
	if (offset < 0 || inside < MIN_TILES || agree < AGREE * inside)
	{
		offset = -1;
		int best = -1;
		for (int k = 0; k < 32; k++)
		{
			int tiles = 0;
			const int matched = agreement(k, tiles);
			if (tiles >= MIN_TILES && matched > best)
			{
				best = matched;
				offset = k;
				inside = tiles;
			}
		}
		agree = best;
	}
	if (offset < 0 || agree < AGREE * inside)
	{
		m_row_offset = -1;
		return false;
	}
	m_row_offset = offset;
	m_agreement = static_cast<double>(agree) / inside;

	// Jumping to somewhere else (dying, a checkpoint, a warp) has the game
	// draw the map again, so what it had changed is forgotten.
	if (m_last_world_x >= 0 && std::abs(world_x - m_last_world_x) > 128)
		m_edits.clear();
	m_last_world_x = world_x;

	scene.active = true;
	scene.ring = false;
	// Plane 1 scrolls with the level: with plane 2's scroll on the first line. (Not with plane 2's
	// on every line: the game scrolls plane 2 away from the level for the lines of the pause
	// banner, see LineRegs::band, and plane 1 stays.)
	scene.plane1 = true;
	for (int line = 0; line < LINES; line++)
		if (frame.line[line].s1x != top.s2x || frame.line[line].s1y != top.s2y)
			scene.plane1 = false;
	scene.world_x = world_x;
	scene.level_width = level_width;
	// The camera's range, and the screen's width beyond its right end.
	scene.min_x = std::max(0, ram.camera_x_start);
	scene.max_x = std::min(level_width, ram.camera_x_end + 160);
	if (scene.max_x <= scene.min_x)
	{
		scene.min_x = 0;
		scene.max_x = level_width;
	}
	// The screen itself is never clipped (the camera and the scroll register
	// differ by a few pixels).
	scene.min_x = std::min(scene.min_x, world_x);
	scene.max_x = std::max(scene.max_x, world_x + 160);
	// The title card covers the level with a picture made for 160 pixels. The
	// level is placed as it is without the card, so that nothing moves when the
	// card goes; the card is held to its own 160 pixels until it starts to move.
	card_state(frame, ram, scene);
	if (!ram.act_clear)
		for (RowTrack &track : m_rows)
			track = RowTrack();
	// Where the camera is locked in an act's end or a boss arena, the level
	// itself is shown beyond the screen (the camera's range would leave it black).
	scene.arena = wide_arena(ram);
	scene.act_clear = ram.act_clear;
	if (scene.arena || ram.act_clear)
	{
		scene.min_x = 0;
		scene.max_x = level_width;
	}
	// On the way to such a place the view has to be as it will be there, or it
	// would slide away from the level's end as the camera nears it and snap back
	// when the camera locks: the level goes 64 pixels past the camera's last
	// position, more than a wide view's side needs, so it is shown.
	else if (arena_level(ram) || plane1_block_level(ram))
		scene.max_x = level_width;

	// In an arena the game moves a boss's body about as a block of plane 1's
	// tiles, by scrolling the plane. The ring is 256 pixels and a wide view
	// 270, and the block leaves the screen into the ring's unseen part: a view
	// wider than the screen would show that part, so the block would also appear
	// at the far side. The ring is shown for one lap only, the one the block is
	// in: the lap is the one that keeps it nearest to where it was last frame.
	// (Only for a block scrolled as a whole: rows that each scroll by their own
	// amount, like the act clear's lines of text, are left as the ring has them.)
	bool uniform = true;
	for (int line = 0; line < LINES; line++)
		if (frame.line[line].s1x != top.s1x)
			uniform = false;
	scene.p1_window = false;
	scene.p1_rows = false;
	// (Not while the zone's title card is up: its text is plane 1's tiles too, and
	// the card is shown as the game draws it, held to its own 160 pixels.)
	const bool block_level = plane1_block_level(ram) && !ram.title_card;
	if (!scene.arena && !block_level)
		m_p1_known = false;
	if (ram.act_clear && !scene.plane1)
	{
		// The act clear's text: each row has its own scroll and its own lap.
		// A row of text is a run of tiles in the ring. It is either in view or in
		// the 96 pixels the screen does not show, and from there it comes in from
		// the left or the right: the ring is round, so there the two look the
		// same, and a wide view that sees 55 pixels of it on each side would show it
		// on both. The lap is the one in which the run is on the screen; a run
		// waiting beyond the screen is in the lap of the side it last came from
		// or went to, and is not drawn until it has moved and that is known.
		scene.p1_window = true;
		scene.p1_rows = true;
		for (int ring_row = 0; ring_row < 32; ring_row++)
		{
			scene.p1_row_lo[ring_row] = k2ge_render::WideScene::HIDDEN_ROW;
			scene.p1_row_s1x[ring_row] = 0;
		}
		bool done[32] = {};
		for (int line = 0; line < LINES; line++)
		{
			const int ring_row = ((frame.line[line].s1y + line) >> 3) & 31;
			if (done[ring_row])
				continue;
			done[ring_row] = true;
			RowTrack &track = m_rows[ring_row];
			const int s1x = frame.line[line].s1x;
			scene.p1_row_s1x[ring_row] = s1x;

			bool occupied[32] = {};
			int count = 0;
			for (int column = 0; column < 32; column++)
			{
				const int at = 0x1000 + ring_row * 64 + column * 2;
				const int tile = frame.vram[at] | (frame.vram[at + 1] << 8);
				occupied[column] = tile != 0 && tile != BLANK;
				count += occupied[column];
			}
			// Which way a row of text moves is only known from a move it made
			// with the text there (the game sets a row's scroll before it draws
			// the text, and that jump says nothing).
			if (count == 0)
			{
				track = RowTrack();
				continue;
			}
			if (track.s1x >= 0)
			{
				const int moved = ((s1x - track.s1x + 128) & 255) - 128;
				if (moved != 0)
					track.direction = moved > 0 ? 1 : -1;
			}
			track.s1x = s1x;
			if (count == 32)
			{
				scene.p1_row_lo[ring_row] = 80 - 128;   // a full row: nothing to tell apart
				continue;
			}
			// The longest empty run of columns ends where the text starts.
			int gap_start = 0, gap = 0;
			for (int start = 0; start < 32; start++)
			{
				if (occupied[start] || !occupied[(start + 31) % 32])
					continue;
				int run = 0;
				while (!occupied[(start + run) % 32])
					run++;
				if (run > gap)
				{
					gap = run;
					gap_start = start;
				}
			}
			const int first = (gap_start + gap) % 32, length = (32 - gap) * 8;
			const int at = mod(first * 8 - s1x, 256);    // screen x of the run's start, in 0..255
			auto overlap = [&](int lap) {
				const int from = at + 256 * lap;
				return std::max(0, std::min(from + length, 160) - std::max(from, 0));
			};
			const int here = overlap(0), left = overlap(-1);
			int lap = 2;
			if (here > 0 && left > 0)
				lap = track.lap != 2 ? track.lap : (here >= left ? 0 : -1);
			else if (here > 0 || left > 0)
				lap = here > 0 ? 0 : -1;
			else if (track.lap != 2)
				lap = track.lap;
			else if (track.direction != 0)
				lap = track.direction > 0 ? 0 : -1;
			track.lap = lap;
			if (lap != 2)
				scene.p1_row_lo[ring_row] = at + 256 * lap + length / 2 - 128;
		}
	}
	else if ((scene.arena || block_level) && !scene.plane1 && uniform)
		p1_block_window(frame, ram, scene, block_level);
	scene.first_col = first_column - WIDE_LEFT_COLS;
	const int first_row = first_row_for(offset);
	for (int ring_row = 0; ring_row < 32; ring_row++)
		scene.row[ring_row] = first_row + (mod(ring_row - first_ring_row + 4, 32) - 4);

	// The game changes the map after drawing it (a collected ring is wiped), so
	// what is on screen is compared with the level data: wherever they differ
	// the change is remembered, to be shown when that tile is beyond the
	// screen; where they agree again it is forgotten.
	auto key = [](int plane, int column, int row) {
		return (static_cast<uint64_t>(plane) << 48) | (static_cast<uint64_t>(column & 0xffffff) << 24) |
				static_cast<uint64_t>(row & 0xffffff);
	};
	for (int plane = 0; plane < 2; plane++)
	{
		if (plane == 0 && !scene.plane1)
			continue;
		const int base = plane ? 0x1800 : 0x1000;
		for (int r = 0; r < SCREEN_ROWS; r++)
		{
			const int ring_row = (first_ring_row + r) & 31;
			for (int c = 0; c < SCREEN_COLS; c++)
			{
				const int column = first_column + c, row = first_row + r;
				if (column < 0 || row < 0 || column / 4 >= header.width || row / 4 >= header.height)
					continue;
				const int at = base + ring_row * 64 + (column & 31) * 2;
				const uint16_t shown = static_cast<uint16_t>(frame.vram[at] | (frame.vram[at + 1] << 8));
				if (shown != entry(header, plane == 1 ? 1 : 0, column, row))
					m_edits[key(plane, column, row)] = shown;
				else
					m_edits.erase(key(plane, column, row));
			}
		}
	}

	// What lies past the level's last column is that column again (see scene_entry)
	// where the view is meant to be able to reach it: in an arena, in the fifth
	// zone's flight (which ends on a platform with the camera stopped 64 pixels
	// short of the data's end, so as to be as it is under the act clear) and under
	// the act clear.
	const bool extend = arena_level(ram) || plane1_block_level(ram) || ram.act_clear;

	// The hardware map is what the game says is on screen, so it is kept for
	// the tiles of the screen itself; everything beyond comes from the level
	// data, as changed by the game.
	for (int plane = 0; plane < 2; plane++)
	{
		const int base = plane ? 0x1800 : 0x1000;
		for (int ring_row = 0; ring_row < 32; ring_row++)
		{
			const int from_top = mod(ring_row - first_ring_row + 4, 32) - 4;   // -4 .. 27
			const int row = first_row + from_top;
			for (int j = 0; j < WIDE_COLS; j++)
			{
				const int column = scene.first_col + j;
				// (With the camera stopped at the end of the level's data, the screen's
				// last, partly shown column can be past it: the game has nothing to draw
				// there, and the view, centred, shows it: it takes the level's last column
				// on, as what lies beyond does.)
				const bool past_data = extend && m_last_column >= 0 && column / 4 > m_last_column;
				const bool on_screen = from_top >= 0 && from_top < SCREEN_ROWS &&
						column >= first_column && column < first_column + SCREEN_COLS && !past_data;
				if (on_screen)
				{
					const int at = base + ring_row * 64 + (column & 31) * 2;
					scene.entry[plane][ring_row][j] = static_cast<uint16_t>(frame.vram[at] | (frame.vram[at + 1] << 8));
				}
				else
				{
					const auto edited = m_edits.find(key(plane, column, row));
					scene.entry[plane][ring_row][j] = edited != m_edits.end() ? edited->second
							: scene_entry(header, plane == 1 ? 1 : 0, column, row, extend);
				}
			}
		}
	}
	return true;
}

} // namespace ngpc
