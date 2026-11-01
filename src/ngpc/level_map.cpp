#include "ngpc/level_map.h"

#include "ngpc/machine.h"

#include <algorithm>
#include <cmath>
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


LevelRam read_level_ram(Machine &machine)
{
	auto word = [&](uint32_t address) { return machine.read_byte(address) | (machine.read_byte(address + 1) << 8); };
	LevelRam ram;
	ram.header = static_cast<uint32_t>(word(LEVEL_HEADER_PTR)) | (static_cast<uint32_t>(word(LEVEL_HEADER_PTR + 2)) << 16);
	ram.camera_x = word(LEVEL_CAMERA_X);
	ram.camera_y = word(LEVEL_CAMERA_Y);
	ram.camera_x_start = word(LEVEL_CAMERA_X_START);
	ram.camera_x_end = word(LEVEL_CAMERA_X_END);
	ram.level = machine.read_byte(LEVEL_NUMBER);

	// The title card and the bosses are tasks of their own: walk the (circular)
	// list.
	uint32_t record = TASK_LIST_HEAD;
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
		if (function >= BOSS_CODE_FIRST && function < BOSS_CODE_END && ram.boss_count < BOSS_TASKS)
			ram.boss_x[ram.boss_count++] = static_cast<int16_t>(word(record + 0x12)) - ram.camera_x;
		record = word(record + TASK_NEXT);
		if (record == TASK_LIST_HEAD)
			break;
	}
	return ram;
}


bool arena_level(const LevelRam &ram)
{
	static constexpr bool ARENA_LEVEL[13] = { true, true, true, true, true, false, true, false, false, false, true, false, false };
	return ram.header != 0 && ram.level >= 0 && ram.level < 13 && ARENA_LEVEL[ram.level];
}


bool wide_arena(const LevelRam &ram)
{
	// Locked: the camera may move less than a screen's width (the end of an
	// act: none; the second zone's boss: 80 pixels).
	return arena_level(ram) && ram.camera_x_end > 0 && ram.camera_x_end - ram.camera_x_start < 160;
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


bool LevelMap::build(const k2ge_render::Frame &frame, const LevelRam &ram, k2ge_render::WideScene &scene)
{
	using namespace k2ge_render;

	scene.active = false;
	m_agreement = 0;
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
	scene.plane1 = true;
	for (int line = 0; line < LINES; line++)
		if (frame.line[line].s1x != frame.line[line].s2x || frame.line[line].s1y != frame.line[line].s2y)
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
	// Where the camera is locked in an act's end or a boss arena, the level
	// itself is shown beyond the screen (the camera's range would leave it black).
	scene.arena = wide_arena(ram);
	if (scene.arena)
	{
		scene.min_x = 0;
		scene.max_x = level_width;
	}
	// On the way to such a place the view has to be as it will be there, or it
	// would slide away from the level's end as the camera nears it and snap back
	// when the camera locks: the level goes 64 pixels past the camera's last
	// position, more than a wide view's side needs, so it is shown.
	else if (arena_level(ram))
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
	if (!scene.arena)
		m_p1_known = false;
	if (scene.arena && !scene.plane1 && uniform)
	{
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
			for (int j = 0; j < ram.boss_count; j++)
			{
				const int lap = centre + 256 * static_cast<int>(std::lround((ram.boss_x[j] - centre) / 256.0));
				if (std::abs(lap - ram.boss_x[j]) < best)
				{
					best = std::abs(lap - ram.boss_x[j]);
					chosen = lap;
				}
			}
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
				const bool on_screen = from_top >= 0 && from_top < SCREEN_ROWS &&
						column >= first_column && column < first_column + SCREEN_COLS;
				if (on_screen)
				{
					const int at = base + ring_row * 64 + (column & 31) * 2;
					scene.entry[plane][ring_row][j] = static_cast<uint16_t>(frame.vram[at] | (frame.vram[at + 1] << 8));
				}
				else
				{
					const auto edited = m_edits.find(key(plane, column, row));
					scene.entry[plane][ring_row][j] = edited != m_edits.end() ? edited->second
							: entry(header, plane == 1 ? 1 : 0, column, row);
				}
			}
		}
	}
	return true;
}

} // namespace ngpc
